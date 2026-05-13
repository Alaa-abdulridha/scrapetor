/*
 * scrapetor_dom.c
 *
 * Native arena-allocated HTML DOM with structural indexes built at parse
 * time.
 *
 *   `Scrapetor::Native::Document.parse(html)` returns a TypedData wrapping
 *   a `dom_doc_t*`. The Ruby Document.backing routes here when the native
 *   extension is loaded; otherwise it falls back to the pure-Ruby DOM in
 *   lib/scrapetor/dom.rb.
 *
 * Architecture
 * ------------
 * • A single malloc'd HTML buffer (the input copy).
 * • One Vec<dom_node_t> arena. Each node is 56 bytes, parent/sibling/child
 *   are uint32_t indices.
 * • One Vec<dom_attr_t> for attribute name/value byte-spans into the html
 *   buffer (zero-copy).
 * • Three open-addressing hash indexes — class, id, tag — built during the
 *   single tokenization pass.
 * • The Ruby wrapper exposes Element-level accessors that take a node id
 *   and dispatch in C. Ruby allocates ZERO objects per DOM node until the
 *   user materializes one.
 *
 * Selectors
 * ---------
 * Reuses the selector vocabulary from scrapetor_native.c (tag, classes,
 * id, attribute matchers, descendant + child combinators). The rightmost
 * atom of a compiled plan is resolved via the indexes (O(1) for class/id
 * lookups), then ancestors are checked right-to-left.
 *
 * This is the file you read first if you want to understand how
 * Scrapetor's read path beats a tree-walking C engine on selector-heavy
 * workloads.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <ruby/thread.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

extern rb_encoding *enc_utf8;  /* defined in scrapetor_native.c */

static VALUE make_utf8_str_cstr(const char *s) {
    VALUE r = rb_str_new_cstr(s);
    rb_enc_associate(r, enc_utf8);
    return r;
}

/* ---- forward decls ------------------------------------------------ */

typedef struct dom_doc dom_doc_t;

void Init_scrapetor_dom(VALUE mod_native);

/* ---- DOM data structures ----------------------------------------- */

#define DOM_TYPE_ELEMENT 1
#define DOM_TYPE_TEXT    3
#define DOM_TYPE_COMMENT 8
#define DOM_TYPE_DOC     9
/* Tombstone for nodes the user has explicitly removed via the native
 * mutation path. element_matches_atom + the child/sibling walks treat
 * tombstoned nodes as if they weren't in the tree, so subsequent
 * queries don't return them — without forcing a fall back to a Ruby
 * Dom view. */
#define DOM_TYPE_REMOVED 0xFE

#define DOM_NIL 0xFFFFFFFFu   /* sentinel for absent index */

#define DOM_INIT_NODES 64
#define DOM_INIT_ATTRS 128

typedef struct {
    uint32_t name_off;
    uint32_t name_len;
    uint32_t val_off;
    uint32_t val_len;
    uint8_t  buf_id;  /* matches owning element's buf_id; replicated here
                       * so attr reads don't have to traverse back to the
                       * element to discover the right buffer. */
} dom_attr_t;

typedef struct {
    uint8_t  type;
    uint8_t  buf_id;  /* 0 = main html_buf; 1..N = extra buffers (fragments
                       * grafted in via dom_node_set_inner_html). All
                       * *_off fields on this node are byte offsets into
                       * d->buf_ptrs[buf_id]. */
    /* Interned tag identifier from the static HTML-tag table
     * (dom_tag_table). 0 = tag not in table (custom element / SVG-uncommon /
     * unknown) — matcher falls back to strncasecmp for those. Lives in the
     * 2-byte alignment slot before `parent`, so adding it cost zero per-node
     * memory. Set at SAX parse time; carried verbatim through persistent-cache
     * serialize/load via the raw node-blob memcpy. */
    uint16_t tag_id;
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next_sibling;
    uint32_t prev_sibling;

    /* element-only */
    uint32_t tag_off;
    uint32_t tag_len;
    uint32_t attr_first;
    uint32_t attr_count;
    /* Cached class/id attribute spans — Lexbor does this on its node
     * struct and the benchmarks show why. Storing the byte-spans we
     * pull out of the attr table at parse time means `class` and `id`
     * lookups during selector matching are O(1) instead of O(attrs). */
    uint32_t class_off;
    uint32_t class_len;
    uint32_t id_off;
    uint32_t id_len;

    /* text/comment only */
    uint32_t text_off;
    uint32_t text_len;

    /* DFS range encoding. Nodes are allocated in pre-order DFS, so the
     * node id IS the pre-order time (dfs_in). dfs_out is the maximum
     * id in the subtree — populated by a single post-pass after the
     * parser finishes. This turns the "is X a descendant of Y" check
     * (used by :has()) from O(subtree) into O(1):
     *   Y.id < X.id <= Y.dfs_out
     * On big subtrees this is what makes :has(.rare-class) competitive
     * with Lexbor instead of dragging behind. */
    uint32_t dfs_out;

    /* Position indices among element siblings. Populated by a single
     * O(children) post-pass after parse so :nth-child / :nth-of-type
     * become O(1) instead of O(n) walks. forward = 1-based from start,
     * rev = 1-based from end. type_* variants count only siblings of
     * the same tag. */
    uint32_t child_idx;
    uint32_t child_idx_rev;
    uint32_t type_idx;
    uint32_t type_idx_rev;

    /* Reserved slot for the ancestor bloom filter optimisation
     * (see TODO in compute_ancestor_blooms). Populated post-parse
     * with one hash bit per ancestor tag/class/id so descendant
     * selectors can fast-reject. Costs zero today (memset 0 in
     * dom_alloc_node); flipped on once the filter is wired up. */
    uint64_t ancestor_bloom;
} dom_node_t;

/* Open-addressing hashmap: string-key -> Vec<u32>.
 *
 * Keys carry an explicit char* pointer + length rather than an offset
 * into a global buffer. Multi-buffer arenas (mutations that graft in
 * fragment HTML via dom_node_set_inner_html) mean nodes can hold byte
 * spans into different Ruby Strings; storing the pointer directly
 * lets the index serve both populations transparently. The pointer
 * remains live because the corresponding buffer's Ruby String stays
 * pinned in d->buf_strs[buf_id]. */

typedef struct {
    const char *key_ptr;
    uint32_t key_len;
    uint32_t *ids;
    uint32_t count;
    uint32_t cap;
    uint32_t key_hash;  /* cached hash */
    uint8_t  used;
} dom_index_entry_t;

typedef struct {
    dom_index_entry_t *buckets;
    size_t cap;
    size_t count;
} dom_index_t;

/* Cap on number of extra buffers (fragments grafted in by
 * dom_node_set_inner_html). 64 is well beyond any realistic mutation
 * pattern — buf_id is uint8_t so the theoretical max is 255 anyway. */
#define DOM_MAX_BUFS 64

struct dom_doc {
    /* Zero-copy input. We hold a reference to a frozen Ruby String and
     * point html_buf at its bytes directly. The mark callback on the
     * typed_data wrapper pins the String so the bytes stay live. */
    VALUE    html_str_value;
    const char *html_buf;
    size_t   html_len;

    /* Parallel arrays indexed by node->buf_id. buf_ptrs[0] always
     * points at html_buf above; entries [1..n_bufs-1] are extras
     * carried in via fragment mutations. buf_strs holds the Ruby
     * String references so they don't get GC'd before the node
     * offsets get read. */
    const char *buf_ptrs[DOM_MAX_BUFS];
    VALUE       buf_strs[DOM_MAX_BUFS];
    int         n_bufs;

    dom_node_t *nodes;
    size_t      n_nodes;
    size_t      cap_nodes;

    dom_attr_t *attrs;
    size_t      n_attrs;
    size_t      cap_attrs;

    dom_index_t class_idx;
    dom_index_t id_idx;
    dom_index_t tag_idx;

    /* Lazy attribute-name index. Maps an attribute name to the list of
     * nodes that carry it. Built on demand — the first query for
     * `[some-attr]` or `[some-attr=...]` pays the O(N) scan; later
     * queries for the same name are O(1). */
    dom_index_t attr_idx;
    int         attr_idx_init;

    uint32_t root_id;       /* doc root id (always 0) */

    /* Lazy parse. `parse(html)` only allocates the shell + freezes the
     * input; the actual tokenisation runs on first query. Pure
     * parse-and-drop workloads never pay the tokenisation cost. */
    int      parsed;
    /* Set on the first dom_node_remove call. The selector fast paths
     * skip a per-candidate "is this REMOVED?" check when this flag is
     * false, so the common case (read-only documents) pays nothing for
     * supporting native mutation. */
    int      has_removed;
    /* Flag flipped on dom_node_set_inner_html. New fragment nodes are
     * appended to the arena at high ids — they don't sit inside the
     * pre-order dfs_in/dfs_out range of their parent any more, so the
     * range-encoding "X descends from Y" check can't be trusted. The
     * matcher honours this by falling back to a parent-walk check
     * after a structural mutation. */
    int      tree_dirty;
    /* Shared selector-result memo. When this Document was instantiated
     * from a parse-cache hit, both fields alias the Ruby Hashes living
     * on the parse cache entry — every Document sharing that entry
     * sees the same Hash. On first mutation we clear cache_disabled so
     * stale (selector, scope_id) → ids hits never return ids that the
     * caller's edits should have invalidated. */
    VALUE    shared_result_cache;
    VALUE    shared_compile_cache;
    int      cache_disabled;
};

/* Per-node / per-attr buffer accessor. Returns the const char* base
 * that node->*_off / attr->*_off offsets are relative to. For
 * unmutated documents (the common case) every node has buf_id == 0
 * and this resolves to d->html_buf with a single array load. */
#define NODE_BUF(d, n) ((d)->buf_ptrs[(n)->buf_id])
#define ATTR_BUF(d, a) ((d)->buf_ptrs[(a)->buf_id])

/* ---- string helpers ---------------------------------------------- */

static inline int ascii_lower_c(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int dom_streq_ci(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if (ascii_lower_c((unsigned char)a[i]) != ascii_lower_c((unsigned char)b[i])) return 0;
    }
    return 1;
}

static uint32_t fnv1a_ci(const char *s, size_t len) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= c;
        h *= 0x01000193u;
    }
    return h;
}

/* ---- static HTML-tag intern table -------------------------------- *
 * Open-addressed perfect-ish lookup over the standard HTML/SVG tag set.
 * The matcher consults n->tag_id (set at parse) vs a->tag_id (set at
 * selector compile); when both are nonzero a single 16-bit equality
 * stands in for an N-byte ASCII-fold strncasecmp.
 *
 * Tags not in this table get id 0 and the matcher falls back to the
 * byte compare — i.e. the fast path is opt-in per-tag rather than
 * required.
 */
typedef struct {
    const char *name;
    uint8_t     len;
    uint16_t    id;
} dom_tag_slot_t;

/* Power-of-2 capacity; FNV1a-ci hash with linear probing. ~150 entries
 * → ~25% load, ~1.1 average probes. */
#define DOM_TAG_TABLE_CAP 512
static dom_tag_slot_t dom_tag_table[DOM_TAG_TABLE_CAP];
static int dom_tag_table_inited = 0;

/* Frequently-referenced ids cached for use in pseudo-class code (e.g.
 * :any-link wants tag in {a, area}). Filled in by dom_tag_table_init. */
static uint16_t DOM_TAG_ID_A    = 0;
static uint16_t DOM_TAG_ID_AREA = 0;

static const char *const DOM_HTML_TAG_NAMES[] = {
    /* HTML5 element set, plus the SVG / MathML names that actually show
     * up in real-world pages (charts, icons, embedded badges). */
    "a", "abbr", "address", "area", "article", "aside", "audio",
    "b", "base", "bdi", "bdo", "blockquote", "body", "br", "button",
    "canvas", "caption", "cite", "code", "col", "colgroup",
    "data", "datalist", "dd", "del", "details", "dfn", "dialog",
    "div", "dl", "dt",
    "em", "embed",
    "fieldset", "figcaption", "figure", "footer", "form",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "head", "header", "hgroup", "hr", "html",
    "i", "iframe", "img", "input", "ins",
    "kbd",
    "label", "legend", "li", "link",
    "main", "map", "mark", "menu", "meta", "meter",
    "nav", "noscript",
    "object", "ol", "optgroup", "option", "output",
    "p", "param", "picture", "pre", "progress",
    "q",
    "rp", "rt", "ruby",
    "s", "samp", "script", "search", "section", "select",
    "slot", "small", "source", "span", "strong", "style",
    "sub", "summary", "sup", "svg",
    "table", "tbody", "td", "template", "textarea", "tfoot",
    "th", "thead", "time", "title", "tr", "track",
    "u", "ul",
    "var", "video",
    "wbr",
    /* SVG / MathML common */
    "circle", "defs", "ellipse", "g", "line", "path",
    "polygon", "polyline", "rect", "text", "tspan", "use",
    "linearGradient", "radialGradient", "stop", "clipPath", "mask", "pattern",
    "filter", "marker", "symbol", "image", "foreignObject",
    NULL
};

static void dom_tag_table_init(void) {
    if (dom_tag_table_inited) return;
    memset(dom_tag_table, 0, sizeof(dom_tag_table));
    uint16_t next_id = 1;
    for (int i = 0; DOM_HTML_TAG_NAMES[i] != NULL; i++) {
        const char *name = DOM_HTML_TAG_NAMES[i];
        size_t len = strlen(name);
        uint32_t h = fnv1a_ci(name, len);
        uint32_t mask = DOM_TAG_TABLE_CAP - 1;
        uint32_t idx = h & mask;
        for (uint32_t k = 0; k < DOM_TAG_TABLE_CAP; k++) {
            uint32_t j = (idx + k) & mask;
            if (dom_tag_table[j].name == NULL) {
                dom_tag_table[j].name = name;
                dom_tag_table[j].len  = (uint8_t)len;
                dom_tag_table[j].id   = next_id;
                if (len == 1 && (name[0] == 'a')) DOM_TAG_ID_A = next_id;
                else if (len == 4 && memcmp(name, "area", 4) == 0) DOM_TAG_ID_AREA = next_id;
                next_id++;
                break;
            }
        }
    }
    dom_tag_table_inited = 1;
}

/* Lookup. Returns 0 (≡ "not in table") for any tag we don't recognise. */
static inline uint16_t dom_intern_tag(const char *p, size_t len) {
    if (len == 0 || len > 32) return 0;
    uint32_t h = fnv1a_ci(p, len);
    uint32_t mask = DOM_TAG_TABLE_CAP - 1;
    uint32_t idx = h & mask;
    for (uint32_t k = 0; k < DOM_TAG_TABLE_CAP; k++) {
        uint32_t j = (idx + k) & mask;
        const dom_tag_slot_t *s = &dom_tag_table[j];
        if (s->name == NULL) return 0;
        if (s->len == len && dom_streq_ci(s->name, s->len, p, len)) return s->id;
    }
    return 0;
}

/* ---- index ops --------------------------------------------------- */

static void dom_index_init(dom_index_t *ix, size_t initial_cap) {
    size_t cap = initial_cap;
    while (cap & (cap - 1)) cap++;       /* round to power of 2 */
    if (cap < 16) cap = 16;
    ix->buckets = (dom_index_entry_t *)calloc(cap, sizeof(dom_index_entry_t));
    ix->cap = cap;
    ix->count = 0;
}

static void dom_index_free(dom_index_t *ix) {
    if (!ix->buckets) return;
    for (size_t i = 0; i < ix->cap; i++) {
        if (ix->buckets[i].used) free(ix->buckets[i].ids);
    }
    free(ix->buckets);
    ix->buckets = NULL;
    ix->cap = 0;
    ix->count = 0;
}

static void dom_index_resize(dom_index_t *ix, size_t new_cap) {
    dom_index_entry_t *old_b = ix->buckets;
    size_t old_cap = ix->cap;
    ix->buckets = (dom_index_entry_t *)calloc(new_cap, sizeof(dom_index_entry_t));
    ix->cap = new_cap;
    ix->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old_b[i].used) continue;
        /* re-insert by raw memory move */
        size_t mask = new_cap - 1;
        size_t k = old_b[i].key_hash & mask;
        while (ix->buckets[k].used) k = (k + 1) & mask;
        ix->buckets[k] = old_b[i];
        ix->count++;
    }
    free(old_b);
}

static dom_index_entry_t *dom_index_get_or_create(dom_index_t *ix, const char *key, size_t klen, uint32_t hash) {
    if (ix->count * 2 > ix->cap) dom_index_resize(ix, ix->cap * 2);
    size_t mask = ix->cap - 1;
    size_t k = hash & mask;
    while (ix->buckets[k].used) {
        dom_index_entry_t *e = &ix->buckets[k];
        if (e->key_hash == hash &&
            dom_streq_ci(e->key_ptr, e->key_len, key, klen)) {
            return e;
        }
        k = (k + 1) & mask;
    }
    dom_index_entry_t *e = &ix->buckets[k];
    e->used = 1;
    e->key_ptr = key;
    e->key_len = (uint32_t)klen;
    e->key_hash = hash;
    e->cap = 4;
    e->count = 0;
    e->ids = (uint32_t *)malloc(sizeof(uint32_t) * e->cap);
    ix->count++;
    return e;
}

static dom_index_entry_t *dom_index_lookup(dom_index_t *ix, const char *key, size_t klen) {
    uint32_t hash = fnv1a_ci(key, klen);
    size_t mask = ix->cap - 1;
    size_t k = hash & mask;
    while (ix->buckets[k].used) {
        dom_index_entry_t *e = &ix->buckets[k];
        if (e->key_hash == hash &&
            dom_streq_ci(e->key_ptr, e->key_len, key, klen)) {
            return e;
        }
        k = (k + 1) & mask;
    }
    return NULL;
}

static void dom_index_push(dom_index_entry_t *e, uint32_t id) {
    if (e->count == e->cap) {
        e->cap *= 2;
        e->ids = (uint32_t *)realloc(e->ids, sizeof(uint32_t) * e->cap);
    }
    e->ids[e->count++] = id;
}

/* ---- arena ops --------------------------------------------------- */

/* Allocate the shell. Caller is responsible for setting html_buf/len
 * and html_str_value before the first parse. Nothing here triggers
 * tokenisation — that's deferred until first query (see ensure_parsed). */
static dom_doc_t *dom_doc_alloc(void) {
    dom_doc_t *d = (dom_doc_t *)calloc(1, sizeof(dom_doc_t));
    d->html_str_value = Qnil;
    d->shared_result_cache = Qnil;
    d->shared_compile_cache = Qnil;
    /* buf_strs initialised to Qnil so the mark callback walks them
     * safely; buf_ptrs[0] gets pointed at html_buf right after
     * dom_parse_html sets html_str_value. */
    for (int _i = 0; _i < DOM_MAX_BUFS; _i++) d->buf_strs[_i] = Qnil;
    d->n_bufs = 1; /* slot 0 reserved for the main html_buf */
    d->cap_nodes = DOM_INIT_NODES;
    d->nodes = (dom_node_t *)malloc(sizeof(dom_node_t) * d->cap_nodes);
    d->cap_attrs = DOM_INIT_ATTRS;
    d->attrs = (dom_attr_t *)malloc(sizeof(dom_attr_t) * d->cap_attrs);
    dom_index_init(&d->class_idx, 32);
    dom_index_init(&d->id_idx,    16);
    dom_index_init(&d->tag_idx,   16);
    d->attr_idx_init = 0;

    /* node 0 = document root, always present */
    dom_node_t *root = &d->nodes[0];
    memset(root, 0, sizeof(*root));
    root->type = DOM_TYPE_DOC;
    root->parent = DOM_NIL;
    root->first_child = DOM_NIL;
    root->last_child = DOM_NIL;
    root->next_sibling = DOM_NIL;
    root->prev_sibling = DOM_NIL;
    root->class_off = DOM_NIL;
    root->id_off = DOM_NIL;
    d->n_nodes = 1;
    d->root_id = 0;
    d->parsed = 0;
    return d;
}

static void dom_doc_free(dom_doc_t *d) {
    if (!d) return;
    /* html_buf is owned by the Ruby String (zero-copy); don't free. */
    free(d->nodes);
    free(d->attrs);
    dom_index_free(&d->class_idx);
    dom_index_free(&d->id_idx);
    dom_index_free(&d->tag_idx);
    if (d->attr_idx_init) dom_index_free(&d->attr_idx);
    free(d);
}

static uint32_t dom_alloc_node(dom_doc_t *d) {
    if (d->n_nodes == d->cap_nodes) {
        d->cap_nodes *= 2;
        d->nodes = (dom_node_t *)realloc(d->nodes, sizeof(dom_node_t) * d->cap_nodes);
    }
    uint32_t id = (uint32_t)d->n_nodes++;
    dom_node_t *n = &d->nodes[id];
    memset(n, 0, sizeof(*n));
    n->parent = DOM_NIL;
    n->first_child = DOM_NIL;
    n->last_child = DOM_NIL;
    n->next_sibling = DOM_NIL;
    n->prev_sibling = DOM_NIL;
    n->attr_first = DOM_NIL;
    n->class_off = DOM_NIL;
    n->id_off    = DOM_NIL;
    return id;
}

static uint32_t dom_alloc_attrs(dom_doc_t *d, uint32_t count) {
    if (count == 0) return DOM_NIL;
    if (d->n_attrs + count > d->cap_attrs) {
        while (d->n_attrs + count > d->cap_attrs) d->cap_attrs *= 2;
        d->attrs = (dom_attr_t *)realloc(d->attrs, sizeof(dom_attr_t) * d->cap_attrs);
    }
    uint32_t start = (uint32_t)d->n_attrs;
    d->n_attrs += count;
    return start;
}

static void dom_append_child(dom_doc_t *d, uint32_t parent_id, uint32_t child_id) {
    dom_node_t *p = &d->nodes[parent_id];
    dom_node_t *c = &d->nodes[child_id];
    c->parent = parent_id;
    if (p->last_child == DOM_NIL) {
        p->first_child = child_id;
        p->last_child = child_id;
        c->prev_sibling = DOM_NIL;
        c->next_sibling = DOM_NIL;
    } else {
        dom_node_t *last = &d->nodes[p->last_child];
        last->next_sibling = child_id;
        c->prev_sibling = p->last_child;
        c->next_sibling = DOM_NIL;
        p->last_child = child_id;
    }
}

/* ---- tokenizer (tailored for DOM building) ----------------------- */

static const char *VOID_TAGS[] = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","source","track","wbr",NULL
};

static int is_void(const char *s, size_t l) {
    for (int i = 0; VOID_TAGS[i]; i++) {
        size_t vl = strlen(VOID_TAGS[i]);
        if (l == vl && strncasecmp(s, VOID_TAGS[i], vl) == 0) return 1;
    }
    return 0;
}

static int is_name_start_byte(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_name_byte(int c) { return is_name_start_byte(c) || (c >= '0' && c <= '9') || c == '-' || c == ':'; }
static int is_ws_byte(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* ---- SIMD byte-class scanners ------------------------------------ *
 * Replace the per-byte advance loops in dom_parse with 16-byte NEON
 * classification on arm64. Three primitives cover the hot SAX paths:
 *   advance_ws         — first non-whitespace
 *   advance_name       — first byte not in [A-Za-z0-9_:-]
 *   advance_attr_end   — first byte in { '=', '>', '/', ws }
 *
 * Each builds a 16-lane mask of "matches the set we want to leave"
 * (or its negation), packs to a u128 via two u64 lane reads, and
 * uses __builtin_ctzll to locate the first set byte. On long stretches
 * of HTML text or attribute-heavy openings this collapses 100+ scalar
 * comparisons into a handful of vector ops.
 *
 * Scalar fallback on non-arm64 keeps the original byte loop. The
 * inline dispatch lets the compiler completely eliminate the unused
 * branch.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#define DOM_HAS_NEON 1
#else
#define DOM_HAS_NEON 0
#endif

#if DOM_HAS_NEON
static inline size_t simd_advance_ws_neon(const char *p, size_t len) {
    size_t i = 0;
    while (i + 16 <= len) {
        uint8x16_t v = vld1q_u8((const uint8_t *)(p + i));
        uint8x16_t e1 = vceqq_u8(v, vdupq_n_u8(' '));
        uint8x16_t e2 = vceqq_u8(v, vdupq_n_u8('\t'));
        uint8x16_t e3 = vceqq_u8(v, vdupq_n_u8('\n'));
        uint8x16_t e4 = vceqq_u8(v, vdupq_n_u8('\r'));
        uint8x16_t e5 = vceqq_u8(v, vdupq_n_u8('\v'));
        uint8x16_t e6 = vceqq_u8(v, vdupq_n_u8('\f'));
        uint8x16_t any = vorrq_u8(vorrq_u8(vorrq_u8(e1, e2), vorrq_u8(e3, e4)),
                                   vorrq_u8(e5, e6));
        uint8x16_t not_ws = vmvnq_u8(any);
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(not_ws), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(not_ws), 1);
        if ((lo | hi) == 0) { i += 16; continue; }
        if (lo) return i + (__builtin_ctzll(lo) >> 3);
        return i + 8 + (__builtin_ctzll(hi) >> 3);
    }
    while (i < len && is_ws_byte((unsigned char)p[i])) i++;
    return i;
}

static inline size_t simd_advance_name_neon(const char *p, size_t len) {
    size_t i = 0;
    while (i + 16 <= len) {
        uint8x16_t v = vld1q_u8((const uint8_t *)(p + i));
        uint8x16_t r_az = vandq_u8(vcgeq_u8(v, vdupq_n_u8('a')), vcleq_u8(v, vdupq_n_u8('z')));
        uint8x16_t r_AZ = vandq_u8(vcgeq_u8(v, vdupq_n_u8('A')), vcleq_u8(v, vdupq_n_u8('Z')));
        uint8x16_t r_09 = vandq_u8(vcgeq_u8(v, vdupq_n_u8('0')), vcleq_u8(v, vdupq_n_u8('9')));
        uint8x16_t r_d  = vceqq_u8(v, vdupq_n_u8('-'));
        uint8x16_t r_c  = vceqq_u8(v, vdupq_n_u8(':'));
        uint8x16_t r_u  = vceqq_u8(v, vdupq_n_u8('_'));
        uint8x16_t is_name = vorrq_u8(vorrq_u8(vorrq_u8(r_az, r_AZ), vorrq_u8(r_09, r_d)),
                                       vorrq_u8(r_c, r_u));
        uint8x16_t not_name = vmvnq_u8(is_name);
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(not_name), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(not_name), 1);
        if ((lo | hi) == 0) { i += 16; continue; }
        if (lo) return i + (__builtin_ctzll(lo) >> 3);
        return i + 8 + (__builtin_ctzll(hi) >> 3);
    }
    while (i < len && is_name_byte((unsigned char)p[i])) i++;
    return i;
}

static inline size_t simd_advance_attr_end_neon(const char *p, size_t len) {
    size_t i = 0;
    while (i + 16 <= len) {
        uint8x16_t v = vld1q_u8((const uint8_t *)(p + i));
        uint8x16_t e_eq = vceqq_u8(v, vdupq_n_u8('='));
        uint8x16_t e_gt = vceqq_u8(v, vdupq_n_u8('>'));
        uint8x16_t e_sl = vceqq_u8(v, vdupq_n_u8('/'));
        uint8x16_t w_sp = vceqq_u8(v, vdupq_n_u8(' '));
        uint8x16_t w_tb = vceqq_u8(v, vdupq_n_u8('\t'));
        uint8x16_t w_nl = vceqq_u8(v, vdupq_n_u8('\n'));
        uint8x16_t w_cr = vceqq_u8(v, vdupq_n_u8('\r'));
        uint8x16_t any = vorrq_u8(vorrq_u8(vorrq_u8(e_eq, e_gt), vorrq_u8(e_sl, w_sp)),
                                   vorrq_u8(vorrq_u8(w_tb, w_nl), w_cr));
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(any), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(any), 1);
        if ((lo | hi) == 0) { i += 16; continue; }
        if (lo) return i + (__builtin_ctzll(lo) >> 3);
        return i + 8 + (__builtin_ctzll(hi) >> 3);
    }
    while (i < len) {
        unsigned char nc = (unsigned char)p[i];
        if (nc == '=' || nc == '>' || nc == '/' || is_ws_byte(nc)) break;
        i++;
    }
    return i;
}
#endif  /* DOM_HAS_NEON */

static inline size_t dom_advance_ws(const char *p, size_t len) {
#if DOM_HAS_NEON
    return simd_advance_ws_neon(p, len);
#else
    size_t i = 0;
    while (i < len && is_ws_byte((unsigned char)p[i])) i++;
    return i;
#endif
}
static inline size_t dom_advance_name(const char *p, size_t len) {
#if DOM_HAS_NEON
    return simd_advance_name_neon(p, len);
#else
    size_t i = 0;
    while (i < len && is_name_byte((unsigned char)p[i])) i++;
    return i;
#endif
}
static inline size_t dom_advance_attr_end(const char *p, size_t len) {
#if DOM_HAS_NEON
    return simd_advance_attr_end_neon(p, len);
#else
    size_t i = 0;
    while (i < len) {
        unsigned char nc = (unsigned char)p[i];
        if (nc == '=' || nc == '>' || nc == '/' || is_ws_byte(nc)) break;
        i++;
    }
    return i;
#endif
}

/* Parse class="…" value and register each space-separated class in
 * the class index, pointing back to node_id. */
static void index_classes(dom_doc_t *d, const char *val, size_t vlen, uint32_t node_id) {
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && is_ws_byte((unsigned char)val[i])) i++;
        size_t s = i;
        while (i < vlen && !is_ws_byte((unsigned char)val[i])) i++;
        if (i > s) {
            uint32_t hash = fnv1a_ci(val + s, i - s);
            dom_index_entry_t *e =
                dom_index_get_or_create(&d->class_idx, val + s, i - s, hash);
            dom_index_push(e, node_id);
        }
    }
}

static void index_id(dom_doc_t *d, const char *val, size_t vlen, uint32_t node_id) {
    if (vlen == 0) return;
    uint32_t hash = fnv1a_ci(val, vlen);
    dom_index_entry_t *e = dom_index_get_or_create(&d->id_idx, val, vlen, hash);
    dom_index_push(e, node_id);
}

static void index_tag(dom_doc_t *d, const char *name, size_t nlen, uint32_t node_id) {
    if (nlen == 0) return;
    uint32_t hash = fnv1a_ci(name, nlen);
    dom_index_entry_t *e = dom_index_get_or_create(&d->tag_idx, name, nlen, hash);
    dom_index_push(e, node_id);
}

/* Parse one HTML document into the arena. The main hot path. */
static void dom_parse(dom_doc_t *d) {
    const char *html = d->html_buf;
    size_t len = d->html_len;
    size_t pos = 0;

    /* Open element stack — indices into d->nodes. The doc root is at sp=0. */
    enum { MAX_STK = 1024 };
    uint32_t stack[MAX_STK];
    int sp = 0;
    stack[sp++] = d->root_id;

    /* Attribute scratch — collected per start-tag then committed. */
    enum { MAX_ATTRS_TAG = 64 };
    dom_attr_t scratch[MAX_ATTRS_TAG];

    while (pos < len) {
        /* Find next '<' */
        size_t text_start = pos;
        const char *lt = (const char *)memchr(html + pos, '<', len - pos);
        size_t lt_pos = lt ? (size_t)(lt - html) : len;

        if (lt_pos > text_start) {
            /* Emit a Text node child of the current frame */
            uint32_t tid = dom_alloc_node(d);
            dom_node_t *t = &d->nodes[tid];
            t->type = DOM_TYPE_TEXT;
            t->text_off = (uint32_t)text_start;
            t->text_len = (uint32_t)(lt_pos - text_start);
            dom_append_child(d, stack[sp - 1], tid);
        }
        pos = lt_pos;
        if (pos >= len) break;

        /* Comment <!-- --> */
        if (pos + 3 < len && html[pos+1] == '!' && html[pos+2] == '-' && html[pos+3] == '-') {
            size_t cstart = pos + 4;
            const char *end = (const char *)memmem(html + cstart, len - cstart, "-->", 3);
            size_t cend = end ? (size_t)(end - html) : len;
            uint32_t cid = dom_alloc_node(d);
            dom_node_t *c = &d->nodes[cid];
            c->type = DOM_TYPE_COMMENT;
            c->text_off = (uint32_t)cstart;
            c->text_len = (uint32_t)(cend - cstart);
            dom_append_child(d, stack[sp - 1], cid);
            pos = end ? cend + 3 : len;
            continue;
        }

        /* Doctype / bogus declaration */
        if (pos + 1 < len && html[pos+1] == '!') {
            const char *gt = (const char *)memchr(html + pos, '>', len - pos);
            pos = gt ? (size_t)(gt - html) + 1 : len;
            continue;
        }

        /* End tag </name> */
        if (pos + 1 < len && html[pos+1] == '/') {
            pos += 2;
            size_t ns = pos;
            pos += dom_advance_name(html + pos, len - pos);
            size_t nlen = pos - ns;
            /* skip to '>' */
            while (pos < len && html[pos] != '>') pos++;
            if (pos < len) pos++;
            /* pop until matching tag, or do nothing if mismatched at root */
            if (nlen > 0) {
                int target = -1;
                for (int i = sp - 1; i > 0; i--) {
                    dom_node_t *n = &d->nodes[stack[i]];
                    if (n->type == DOM_TYPE_ELEMENT && n->tag_len == nlen &&
                        strncasecmp(html + n->tag_off, html + ns, nlen) == 0) {
                        target = i; break;
                    }
                }
                if (target > 0) sp = target;
            }
            continue;
        }

        /* Start tag */
        if (pos + 1 < len && is_name_start_byte((unsigned char)html[pos+1])) {
            pos++; /* skip '<' */
            size_t ns = pos;
            pos += dom_advance_name(html + pos, len - pos);
            size_t nlen = pos - ns;
            if (nlen == 0) continue;
            const char *tag_p = html + ns;

            uint32_t n_attrs = 0;
            const char *cls_p = NULL; size_t cls_len = 0;
            const char *id_p  = NULL; size_t id_len  = 0;
            int self_closing = 0;

            /* attributes */
            while (pos < len) {
                pos += dom_advance_ws(html + pos, len - pos);
                if (pos >= len) break;
                char ch = html[pos];
                if (ch == '>') { pos++; break; }
                if (ch == '/' && pos + 1 < len && html[pos+1] == '>') {
                    self_closing = 1; pos += 2; break;
                }
                /* attr name */
                size_t an_s = pos;
                pos += dom_advance_attr_end(html + pos, len - pos);
                size_t an_len = pos - an_s;
                size_t av_s = 0, av_len = 0;
                pos += dom_advance_ws(html + pos, len - pos);
                if (pos < len && html[pos] == '=') {
                    pos++;
                    pos += dom_advance_ws(html + pos, len - pos);
                    if (pos < len) {
                        char q = html[pos];
                        if (q == '"' || q == '\'') {
                            pos++;
                            av_s = pos;
                            while (pos < len && html[pos] != q) pos++;
                            av_len = pos - av_s;
                            if (pos < len) pos++;
                        } else {
                            av_s = pos;
                            while (pos < len && !is_ws_byte((unsigned char)html[pos]) && html[pos] != '>') pos++;
                            av_len = pos - av_s;
                        }
                    }
                }
                if (an_len == 0) continue;
                if (n_attrs < MAX_ATTRS_TAG) {
                    scratch[n_attrs].name_off = (uint32_t)an_s;
                    scratch[n_attrs].name_len = (uint32_t)an_len;
                    scratch[n_attrs].val_off  = (uint32_t)av_s;
                    scratch[n_attrs].val_len  = (uint32_t)av_len;
                    scratch[n_attrs].buf_id   = 0;
                    n_attrs++;
                }
                if (an_len == 5 && strncasecmp(html + an_s, "class", 5) == 0) {
                    cls_p = html + av_s; cls_len = av_len;
                } else if (an_len == 2 && strncasecmp(html + an_s, "id", 2) == 0) {
                    id_p = html + av_s; id_len = av_len;
                }
            }

            /* Allocate the element */
            uint32_t eid = dom_alloc_node(d);
            dom_node_t *e = &d->nodes[eid];
            e->type = DOM_TYPE_ELEMENT;
            e->tag_off = (uint32_t)ns;
            e->tag_len = (uint32_t)nlen;
            e->tag_id  = dom_intern_tag(tag_p, nlen);
            e->attr_count = n_attrs;
            e->attr_first = (n_attrs > 0) ? dom_alloc_attrs(d, n_attrs) : DOM_NIL;
            if (n_attrs > 0) {
                memcpy(&d->attrs[e->attr_first], scratch, sizeof(dom_attr_t) * n_attrs);
            }
            /* Cache class/id spans on the node — the same data the
             * indexes were built from. Lets selector matching read them
             * in O(1) instead of re-scanning attrs. */
            if (cls_p) {
                e->class_off = (uint32_t)(cls_p - html);
                e->class_len = (uint32_t)cls_len;
            }
            if (id_p) {
                e->id_off = (uint32_t)(id_p - html);
                e->id_len = (uint32_t)id_len;
            }
            dom_append_child(d, stack[sp - 1], eid);

            /* Indexes */
            index_tag(d, tag_p, nlen, eid);
            if (cls_p) index_classes(d, cls_p, cls_len, eid);
            if (id_p)  index_id(d, id_p, id_len, eid);

            /* Raw text for script/style: skip content until matching close. */
            int is_script = (nlen == 6 && strncasecmp(tag_p, "script", 6) == 0);
            int is_style  = (nlen == 5 && strncasecmp(tag_p, "style", 5) == 0);
            if (is_script || is_style) {
                size_t rstart = pos;
                const char *needle = is_script ? "</script" : "</style";
                size_t nl = is_script ? 8 : 7;
                while (pos < len) {
                    const char *next = (const char *)memchr(html + pos, '<', len - pos);
                    if (!next) { pos = len; break; }
                    size_t p = (size_t)(next - html);
                    if (p + 1 + nl < len && html[p + 1] == '/' &&
                        strncasecmp(html + p + 2, needle + 2, nl - 2) == 0) {
                        /* emit text child for raw content */
                        if (p > rstart) {
                            uint32_t tid = dom_alloc_node(d);
                            dom_node_t *t = &d->nodes[tid];
                            t->type = DOM_TYPE_TEXT;
                            t->text_off = (uint32_t)rstart;
                            t->text_len = (uint32_t)(p - rstart);
                            dom_append_child(d, eid, tid);
                        }
                        pos = p;
                        while (pos < len && html[pos] != '>') pos++;
                        if (pos < len) pos++;
                        break;
                    }
                    pos = p + 1;
                }
                /* element ended with the close tag — don't push */
                continue;
            }

            int void_el = is_void(tag_p, nlen);
            if (!void_el && !self_closing) {
                if (sp < MAX_STK) stack[sp++] = eid;
            }
            continue;
        }

        /* Bogus '<': emit literal */
        {
            uint32_t tid = dom_alloc_node(d);
            dom_node_t *t = &d->nodes[tid];
            t->type = DOM_TYPE_TEXT;
            t->text_off = (uint32_t)pos;
            t->text_len = 1;
            dom_append_child(d, stack[sp - 1], tid);
        }
        pos++;
    }
}

/* ---- text accumulation ------------------------------------------- */

/* Encode codepoint cp as UTF-8 into out (>=4 bytes). Returns byte count.
 * Replaces ill-formed / out-of-range codepoints with U+FFFD. */
static int utf8_encode_cp(unsigned int cp, char *out) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        out[0] = (char)0xEF; out[1] = (char)0xBF; out[2] = (char)0xBD;
        return 3;
    }
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Append the decoded form of [p, p+L) to buf, expanding the minimal
 * named-entity set plus numeric `&#NNN;` / `&#xHH;` references. Anything
 * unrecognised passes through literally. */
static void append_decoded(const char *p, size_t L, VALUE buf) {
    size_t i = 0, start = 0;
    while (i < L) {
        if (p[i] == '&') {
            if (i > start) rb_str_buf_cat(buf, p + start, i - start);
            size_t j = i + 1;
            size_t cap = (L - j < 12) ? (L - j) : 12;
            while (j < i + 1 + cap && p[j] != ';' && p[j] != '&' && p[j] != ' ' && p[j] != '<') j++;
            int matched = 0;
            if (j < L && p[j] == ';') {
                size_t elen = j - i - 1;
                const char *e = p + i + 1;
                char rep[8]; int rl = 0;
                if (elen >= 2 && e[0] == '#') {
                    unsigned int cp = 0;
                    int ok = 1;
                    if (e[1] == 'x' || e[1] == 'X') {
                        if (elen < 3) ok = 0;
                        for (size_t k = 2; ok && k < elen; k++) {
                            char c = e[k];
                            cp <<= 4;
                            if      (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                            else ok = 0;
                        }
                    } else {
                        for (size_t k = 1; ok && k < elen; k++) {
                            char c = e[k];
                            if (c < '0' || c > '9') { ok = 0; break; }
                            cp = cp * 10 + (unsigned)(c - '0');
                        }
                    }
                    if (ok) { rl = utf8_encode_cp(cp, rep); }
                } else if (elen == 3 && memcmp(e, "amp", 3) == 0)  { rep[0] = '&'; rl = 1; }
                else if (elen == 2 && memcmp(e, "lt", 2) == 0)     { rep[0] = '<'; rl = 1; }
                else if (elen == 2 && memcmp(e, "gt", 2) == 0)     { rep[0] = '>'; rl = 1; }
                else if (elen == 4 && memcmp(e, "quot", 4) == 0)   { rep[0] = '"'; rl = 1; }
                else if (elen == 4 && memcmp(e, "apos", 4) == 0)   { rep[0] = '\''; rl = 1; }
                else if (elen == 4 && memcmp(e, "nbsp", 4) == 0)   { rep[0] = ' '; rl = 1; }
                if (rl > 0) {
                    rb_str_buf_cat(buf, rep, rl);
                    i = j + 1; start = i; matched = 1;
                }
            }
            if (!matched) {
                rb_str_buf_cat(buf, "&", 1);
                i++; start = i;
            }
        } else {
            i++;
        }
    }
    if (i > start) rb_str_buf_cat(buf, p + start, i - start);
}

/* Append the decoded textual content of a subtree to `buf`. */
static void append_subtree_text(dom_doc_t *d, uint32_t nid, VALUE buf) {
    dom_node_t *n = &d->nodes[nid];
    if (n->type == DOM_TYPE_TEXT) {
        append_decoded(NODE_BUF(d, n) + n->text_off, n->text_len, buf);
        return;
    }
    if (n->type != DOM_TYPE_ELEMENT && n->type != DOM_TYPE_DOC) return;
    uint32_t c = n->first_child;
    while (c != DOM_NIL) {
        append_subtree_text(d, c, buf);
        c = d->nodes[c].next_sibling;
    }
}

/* Look up (or build on first use) the candidate list of element ids
 * carrying a given attribute name. Lets selectors like `[data-sku=…]`
 * skip the universal-element fallback. The index is populated on
 * demand for the specific attribute names actually queried — we never
 * build indexes for attributes nobody asks about. */
static dom_index_entry_t *
dom_attr_candidates(dom_doc_t *d, const char *name, size_t nlen) {
    if (!d->attr_idx_init) {
        dom_index_init(&d->attr_idx, 8);
        d->attr_idx_init = 1;
    }
    /* class / id have token / single-value indexes (class_idx, id_idx)
     * but those don't help when the selector is a full-attribute-value
     * match like `[class="hello world"]` or `[class="L'appareil"]` —
     * the dedicated indexes are keyed on per-class tokens / unique ids,
     * not the raw attribute string. Build the generic attribute index
     * for them too so the bracket-form selectors get a candidate set. */

    uint32_t hash = fnv1a_ci(name, nlen);
    /* Probe — if we've built this name's bucket already, return it. */
    {
        size_t mask = d->attr_idx.cap - 1;
        size_t k = hash & mask;
        while (d->attr_idx.buckets[k].used) {
            dom_index_entry_t *e = &d->attr_idx.buckets[k];
            if (e->key_hash == hash &&
                dom_streq_ci(e->key_ptr, e->key_len, name, nlen)) {
                return e;
            }
            k = (k + 1) & mask;
        }
    }

    /* Not built. Scan once, populate, return the bucket.
     *
     * We don't have a stable html_buf offset for `name` (it came from
     * Ruby), so we temporarily store the name in a malloc'd scratch
     * key — but the existing index API expects keys to live in html_buf.
     * To keep things uniform, scan first to find at least one element
     * carrying the attribute and steal its name offset. If no element
     * has it, register an empty bucket using a sentinel key. */
    uint32_t name_off = DOM_NIL;
    for (uint32_t i = 0; i < d->n_nodes; i++) {
        dom_node_t *nd = &d->nodes[i];
        if (nd->type != DOM_TYPE_ELEMENT) continue;
        for (uint32_t k = 0; k < nd->attr_count; k++) {
            dom_attr_t *ax = &d->attrs[nd->attr_first + k];
            if (ax->name_len == nlen &&
                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, name, nlen) == 0) {
                name_off = ax->name_off;
                goto found_name;
            }
        }
    }
    /* No element carries this attribute name at all — register an
     * empty bucket using a zero-length key so subsequent lookups
     * short-circuit immediately. */
    {
        dom_index_entry_t *empty = dom_index_get_or_create(
            &d->attr_idx, d->html_buf, 0, fnv1a_ci("", 0));
        empty->key_len = 0;
        return empty;
    }

found_name: {
        dom_index_entry_t *e = dom_index_get_or_create(
            &d->attr_idx, d->html_buf + name_off, nlen, hash);
        for (uint32_t i = 0; i < d->n_nodes; i++) {
            dom_node_t *nd = &d->nodes[i];
            if (nd->type != DOM_TYPE_ELEMENT) continue;
            for (uint32_t k = 0; k < nd->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[nd->attr_first + k];
                if (ax->name_len == nlen &&
                    strncasecmp(ATTR_BUF(d, ax) + ax->name_off, name, nlen) == 0) {
                    dom_index_push(e, i);
                    break;
                }
            }
        }
        return e;
    }
}

/* ---- Ruby wrapper ----------------------------------------------- */

static void dom_doc_typed_mark(void *ptr) {
    dom_doc_t *d = (dom_doc_t *)ptr;
    /* Pin every Ruby String backing a buf slot. Includes the main
     * html_str_value (slot 0) and every fragment buffer added via
     * dom_node_set_inner_html. */
    if (!d) return;
    if (d->html_str_value != Qnil && d->html_str_value != 0) {
        rb_gc_mark(d->html_str_value);
    }
    for (int i = 0; i < d->n_bufs; i++) {
        if (d->buf_strs[i] != Qnil && d->buf_strs[i] != 0) {
            rb_gc_mark(d->buf_strs[i]);
        }
    }
}

static void dom_doc_typed_free(void *ptr) {
    dom_doc_free((dom_doc_t *)ptr);
}

static size_t dom_doc_typed_size(const void *ptr) {
    const dom_doc_t *d = (const dom_doc_t *)ptr;
    return sizeof(dom_doc_t) +
           d->cap_nodes * sizeof(dom_node_t) +
           d->cap_attrs * sizeof(dom_attr_t);
}

static const rb_data_type_t dom_doc_data_type = {
    "Scrapetor::Native::Document",
    { dom_doc_typed_mark, dom_doc_typed_free, dom_doc_typed_size, },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

/* Run the tokeniser if it hasn't run yet. Called from every read path
 * — so from the outside the Document looks fully parsed, but parse-
 * and-drop workloads never pay the cost. */
/* After parse, compute the dfs_out timestamp on every node by sweeping
 * backwards through the arena. Nodes are allocated in pre-order, so
 * each node's id is its dfs_in; its dfs_out is the highest id in its
 * subtree. Walking last-to-first and pushing the max id up to each
 * parent populates the field in one O(n) pass. */
static void compute_dfs_out(dom_doc_t *d) {
    for (uint32_t i = 0; i < d->n_nodes; i++) {
        d->nodes[i].dfs_out = i;
    }
    if (d->n_nodes == 0) return;
    for (uint32_t i = d->n_nodes - 1; i > 0; i--) {
        uint32_t p = d->nodes[i].parent;
        if (p != DOM_NIL && d->nodes[p].dfs_out < d->nodes[i].dfs_out) {
            d->nodes[p].dfs_out = d->nodes[i].dfs_out;
        }
    }
}

/* After parse, walk each parent and assign 1-based position indices to
 * its element children (forward + reverse, and per-tag for nth-of-type).
 * Linear in total children with a 32-entry tag cache per parent — typical
 * pages have well under 32 distinct child-tag names per parent, so the
 * inner search stays effectively constant. Memoising these positions at
 * parse time collapses :nth-child / :nth-of-type evaluation from an
 * O(n²) sibling walk per query into a single field read. */
static void compute_position_indices(dom_doc_t *d) {
    /* Per-parent tag cache. 32 unique tags is generous — divs holding
     * SERP-style result rows rarely exceed 4-5 distinct child tag names. */
    struct {
        uint32_t off;
        uint32_t len;
        uint32_t total;
        uint32_t seen;
    } tag_slots[32];

    for (uint32_t p = 0; p < d->n_nodes; p++) {
        uint8_t pt = d->nodes[p].type;
        if (pt != DOM_TYPE_ELEMENT && pt != DOM_TYPE_DOC) continue;
        uint32_t fc = d->nodes[p].first_child;
        if (fc == DOM_NIL) continue;

        /* Pass 1: count element children, total per unique tag. */
        uint32_t total_elements = 0;
        uint32_t n_slots = 0;
        for (uint32_t c = fc; c != DOM_NIL; c = d->nodes[c].next_sibling) {
            if (d->nodes[c].type != DOM_TYPE_ELEMENT) continue;
            total_elements++;
            uint32_t off = d->nodes[c].tag_off;
            uint32_t len = d->nodes[c].tag_len;
            int hit = 0;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (tag_slots[s].len == len &&
                    strncasecmp(d->html_buf + tag_slots[s].off,
                                d->html_buf + off, len) == 0) {
                    tag_slots[s].total++;
                    hit = 1; break;
                }
            }
            if (!hit && n_slots < 32) {
                tag_slots[n_slots].off = off;
                tag_slots[n_slots].len = len;
                tag_slots[n_slots].total = 1;
                tag_slots[n_slots].seen  = 0;
                n_slots++;
            }
        }

        /* Pass 2: assign forward + reverse indices in one walk. */
        uint32_t fwd = 1;
        for (uint32_t c = fc; c != DOM_NIL; c = d->nodes[c].next_sibling) {
            if (d->nodes[c].type != DOM_TYPE_ELEMENT) continue;
            d->nodes[c].child_idx     = fwd;
            d->nodes[c].child_idx_rev = total_elements - fwd + 1;
            fwd++;
            uint32_t off = d->nodes[c].tag_off;
            uint32_t len = d->nodes[c].tag_len;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (tag_slots[s].len == len &&
                    strncasecmp(d->html_buf + tag_slots[s].off,
                                d->html_buf + off, len) == 0) {
                    tag_slots[s].seen++;
                    d->nodes[c].type_idx     = tag_slots[s].seen;
                    d->nodes[c].type_idx_rev = tag_slots[s].total - tag_slots[s].seen + 1;
                    break;
                }
            }
        }
    }
}

/* ---- ancestor bloom filter --------------------------------------- *
 * Each element node carries a 64-bit bloom (`ancestor_bloom`) of every
 * tag-name, class-name, and id-name appearing on any of its strict
 * ancestors. For a selector chain `A B` (descendant), `A B C`, ... we
 * accumulate the bloom signature of the non-rightmost atoms into a
 * single `required` mask at plan compile, then fast-reject a candidate
 * `(d->nodes[id].ancestor_bloom & required) != required`. False
 * positives are fine (we fall back to the existing parent-walk);
 * false negatives must never occur, so the hash is content-based and
 * identical on both the parse-time signature and the selector-time
 * predicate.
 *
 * Hash:    fnv1a_ci on the name bytes.
 * Bits:    two bits per signature, distributing using two slices of
 *          the same 32-bit FNV hash. 2 bits per item @ 64 buckets
 *          stays well-below saturation for typical SERP pages
 *          (≈ 25 distinct ancestor tags/classes/ids → ≈ 50 bits set,
 *          per-bit P(set) ≈ 0.54, FPR per 2-bit lookup ≈ 0.29).
 *
 * Cost:    a single linear pass at parse time (O(N)), and a 64-bit
 *          mask AND per candidate at query time.
 */
static inline uint64_t dom_bloom_bits_ci(const char *p, size_t len) {
    if (len == 0) return 0;
    uint32_t h = fnv1a_ci(p, len);
    return (1ull << (h & 63)) | (1ull << ((h >> 11) & 63));
}

static inline uint64_t dom_node_self_bloom(dom_doc_t *d, const dom_node_t *n) {
    uint64_t b = 0;
    if (n->tag_len > 0) {
        b |= dom_bloom_bits_ci(NODE_BUF(d, n) + n->tag_off, n->tag_len);
    }
    if (n->class_off != DOM_NIL && n->class_len > 0) {
        const char *p = NODE_BUF(d, n) + n->class_off;
        size_t L = n->class_len;
        size_t s = 0;
        for (size_t k = 0; k <= L; k++) {
            if (k == L || is_ws_byte((unsigned char)p[k])) {
                if (k > s) b |= dom_bloom_bits_ci(p + s, k - s);
                s = k + 1;
            }
        }
    }
    if (n->id_off != DOM_NIL && n->id_len > 0) {
        b |= dom_bloom_bits_ci(NODE_BUF(d, n) + n->id_off, n->id_len);
    }
    return b;
}

/* O(N) — nodes are allocated in pre-order DFS so the parent's
 * ancestor_bloom + self_bloom is always available when we reach a
 * child. Non-element nodes inherit nothing (their bloom is unused). */
static void compute_ancestor_blooms(dom_doc_t *d) {
    for (uint32_t i = 0; i < d->n_nodes; i++) {
        dom_node_t *n = &d->nodes[i];
        uint32_t p = n->parent;
        if (p == DOM_NIL) {
            n->ancestor_bloom = 0;
        } else {
            dom_node_t *pn = &d->nodes[p];
            n->ancestor_bloom = pn->ancestor_bloom | dom_node_self_bloom(d, pn);
        }
    }
}

/* ---- parse cache ------------------------------------------------- *
 * Same HTML parsed N times → parse once, memcpy the node/attr blobs
 * thereafter. The indexes (class_idx/id_idx/tag_idx) are rebuilt
 * cheaply by walking the cached nodes — orders of magnitude faster
 * than tokenising the source again. Cache key is FNV-1a hash of the
 * full HTML content plus length. Capped LRU.
 */
typedef struct {
    uint64_t      hash;
    size_t        html_len;
    /* Snapshot of the parsed arena. Owned by the cache entry — copied
     * into each fresh document on cache hit. */
    dom_node_t   *nodes;
    size_t        n_nodes;
    dom_attr_t   *attrs;
    size_t        n_attrs;
    uint32_t      root_id;
    uint64_t      last_used;  /* monotonic for LRU */
    /* Cross-Document memoisation. Documents that hit this slot share
     * the same Ruby Hash so a second iteration through identical HTML
     * skips both the SAX phase AND the selector-matching phase: each
     * (selector_string, scope_id) lookup that was computed on a prior
     * iteration is returned directly. Registered with the Ruby GC so
     * the table survives across native-Document lifetimes. */
    VALUE         result_cache;
    VALUE         compile_cache;
} parse_cache_entry_t;

#define PARSE_CACHE_SLOTS 16
static parse_cache_entry_t g_parse_cache[PARSE_CACHE_SLOTS];
static uint64_t g_parse_cache_clock = 0;

static uint64_t fnv1a_hash(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Find a cache entry whose key (hash + length) matches. Caller compares
 * full content on hit since hash collisions are possible. */
static parse_cache_entry_t *parse_cache_lookup(uint64_t h, size_t len) {
    for (int i = 0; i < PARSE_CACHE_SLOTS; i++) {
        if (g_parse_cache[i].nodes &&
            g_parse_cache[i].hash == h &&
            g_parse_cache[i].html_len == len) {
            g_parse_cache[i].last_used = ++g_parse_cache_clock;
            return &g_parse_cache[i];
        }
    }
    return NULL;
}

/* Pick an LRU slot to evict. Returns a slot pointer; caller frees its
 * existing nodes/attrs if any. */
static parse_cache_entry_t *parse_cache_evict_slot(void) {
    int best_i = 0;
    uint64_t best_age = UINT64_MAX;
    for (int i = 0; i < PARSE_CACHE_SLOTS; i++) {
        if (g_parse_cache[i].nodes == NULL) return &g_parse_cache[i];
        if (g_parse_cache[i].last_used < best_age) {
            best_age = g_parse_cache[i].last_used;
            best_i = i;
        }
    }
    parse_cache_entry_t *e = &g_parse_cache[best_i];
    free(e->nodes); e->nodes = NULL;
    free(e->attrs); e->attrs = NULL;
    if (!NIL_P(e->result_cache)) {
        rb_gc_unregister_address(&e->result_cache);
        e->result_cache = Qnil;
    }
    if (!NIL_P(e->compile_cache)) {
        rb_gc_unregister_address(&e->compile_cache);
        e->compile_cache = Qnil;
    }
    return e;
}

/* Walk the parsed nodes and rebuild the (still-empty) class/id/tag
 * structural indexes. The class/id offsets on each node were preserved
 * by the memcpy from cache, so this is purely an index-fill — no
 * re-tokenisation. */
static void rebuild_indexes_from_nodes(dom_doc_t *d) {
    for (uint32_t i = 1; i < d->n_nodes; i++) {
        dom_node_t *n = &d->nodes[i];
        if (n->type != DOM_TYPE_ELEMENT) continue;

        if (n->class_off != DOM_NIL && n->class_len > 0) {
            const char *val = NODE_BUF(d, n) + n->class_off;
            size_t vlen = n->class_len;
            size_t s = 0;
            for (size_t k = 0; k <= vlen; k++) {
                if (k == vlen || is_ws_byte((unsigned char)val[k])) {
                    if (k > s) {
                        uint32_t h = fnv1a_ci(val + s, k - s);
                        dom_index_entry_t *e = dom_index_get_or_create(
                            &d->class_idx, val + s, k - s, h);
                        dom_index_push(e, i);
                    }
                    s = k + 1;
                }
            }
        }
        if (n->id_off != DOM_NIL && n->id_len > 0) {
            const char *val = NODE_BUF(d, n) + n->id_off;
            uint32_t h = fnv1a_ci(val, n->id_len);
            dom_index_entry_t *e = dom_index_get_or_create(
                &d->id_idx, val, n->id_len, h);
            dom_index_push(e, i);
        }
        if (n->tag_len > 0) {
            const char *name = NODE_BUF(d, n) + n->tag_off;
            uint32_t h = fnv1a_ci(name, n->tag_len);
            dom_index_entry_t *e = dom_index_get_or_create(
                &d->tag_idx, name, n->tag_len, h);
            dom_index_push(e, i);
        }
    }
}

static void ensure_parsed(dom_doc_t *d) {
    if (d->parsed) return;
    d->parsed = 1;  /* set before parse so we don't re-enter on error */

    /* Try the parse cache first. Workloads that loop the same HTML
     * through Document.parse repeatedly (test fixtures, idempotent
     * extraction pipelines) skip tokenisation entirely on second-
     * and-later iterations. */
    uint64_t key_hash = fnv1a_hash(d->html_buf, d->html_len);
    parse_cache_entry_t *hit = parse_cache_lookup(key_hash, d->html_len);
    if (hit) {
        /* Grow arena to fit the cached blob. */
        if (d->cap_nodes < hit->n_nodes) {
            d->cap_nodes = hit->n_nodes;
            d->nodes = (dom_node_t *)realloc(d->nodes, sizeof(dom_node_t) * d->cap_nodes);
        }
        if (d->cap_attrs < hit->n_attrs) {
            d->cap_attrs = hit->n_attrs;
            d->attrs = (dom_attr_t *)realloc(d->attrs, sizeof(dom_attr_t) * d->cap_attrs);
        }
        memcpy(d->nodes, hit->nodes, sizeof(dom_node_t) * hit->n_nodes);
        memcpy(d->attrs, hit->attrs, sizeof(dom_attr_t) * hit->n_attrs);
        d->n_nodes = hit->n_nodes;
        d->n_attrs = hit->n_attrs;
        d->root_id = hit->root_id;
        d->shared_result_cache  = hit->result_cache;
        d->shared_compile_cache = hit->compile_cache;
        rebuild_indexes_from_nodes(d);
        compute_dfs_out(d);
        compute_position_indices(d);
        compute_ancestor_blooms(d);
        return;
    }

    dom_parse(d);
    compute_dfs_out(d);
    compute_position_indices(d);
    compute_ancestor_blooms(d);

    /* Store in cache. nodes/attrs blobs are duped so the cache outlives
     * the source Document. The Ruby String backing html_buf is frozen
     * and shared by reference; the cached node offsets target byte-
     * identical content in every future Document so the pointer
     * substitution at hit time is safe. */
    parse_cache_entry_t *slot = parse_cache_evict_slot();
    slot->hash = key_hash;
    slot->html_len = d->html_len;
    slot->n_nodes = d->n_nodes;
    slot->n_attrs = d->n_attrs;
    slot->root_id = d->root_id;
    slot->nodes = (dom_node_t *)malloc(sizeof(dom_node_t) * d->n_nodes);
    memcpy(slot->nodes, d->nodes, sizeof(dom_node_t) * d->n_nodes);
    if (d->n_attrs > 0) {
        slot->attrs = (dom_attr_t *)malloc(sizeof(dom_attr_t) * d->n_attrs);
        memcpy(slot->attrs, d->attrs, sizeof(dom_attr_t) * d->n_attrs);
    } else {
        slot->attrs = (dom_attr_t *)malloc(sizeof(dom_attr_t));
    }
    /* Allocate the shared memo + compile-plan caches and pin them so
     * the GC doesn't reclaim them while subsequent Documents hold a
     * reference. eviction unpins. */
    slot->result_cache  = rb_hash_new();
    slot->compile_cache = rb_hash_new();
    rb_gc_register_address(&slot->result_cache);
    rb_gc_register_address(&slot->compile_cache);
    d->shared_result_cache  = slot->result_cache;
    d->shared_compile_cache = slot->compile_cache;
    slot->last_used = ++g_parse_cache_clock;
}

static dom_doc_t *get_dom(VALUE self) {
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    ensure_parsed(d);
    return d;
}

/* Variant for methods that don't actually depend on parsed state
 * (currently unused but kept for clarity if we add such methods). */
static dom_doc_t *get_dom_raw(VALUE self) {
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    return d;
}

/* class methods */

static VALUE dom_parse_html(VALUE klass, VALUE html_v) {
    Check_Type(html_v, T_STRING);

    /* Dup-and-freeze the input so external mutations can't corrupt our
     * byte spans, then point at the frozen copy's bytes directly. The
     * dup is CoW in Ruby 3+, so it doesn't actually copy the buffer
     * unless someone tries to mutate it later. */
    VALUE owned = rb_str_dup(html_v);
    rb_obj_freeze(owned);

    dom_doc_t *d = dom_doc_alloc();
    d->html_str_value = owned;
    d->html_buf = RSTRING_PTR(owned);
    /* buf slot 0 aliases html_buf — any node with buf_id=0 (the
     * common case for parser-allocated nodes) reads through here. */
    d->buf_ptrs[0] = d->html_buf;
    d->buf_strs[0] = owned;
    d->html_len = (size_t)RSTRING_LEN(owned);

    /* Tokenisation deferred — ensure_parsed runs it on first query. */
    return TypedData_Wrap_Struct(klass, &dom_doc_data_type, d);
}

/* ---- parallel parse (GVL-released, pthread workers) -------------- *
 * The single-threaded parse + index build is pure C — it touches no
 * Ruby VALUEs, only the dom_doc_t arena and libc malloc. That means
 * we can split a batch of N documents across pthread workers, release
 * the GVL for the duration of the parse phase, and re-acquire it once
 * at the end to wrap each completed arena in a Document.
 *
 * Bypasses the in-memory parse cache (g_parse_cache) because that
 * structure holds VALUEs and is mutated under GVL elsewhere — touching
 * it from a no-GVL thread would race. Workloads that benefit from the
 * cache (same HTML parsed repeatedly) should use the serial parse
 * path; workloads that benefit from parallelism (a batch of distinct
 * documents to chew through) use this one.
 */
static void dom_parse_eager_nocache(dom_doc_t *d) {
    if (d->parsed) return;
    d->parsed = 1;
    dom_parse(d);
    compute_dfs_out(d);
    compute_position_indices(d);
    compute_ancestor_blooms(d);
}

typedef struct {
    dom_doc_t **docs;
    size_t      n_docs;
    /* Atomic claim counter — each worker grabs the next index via
     * __atomic_fetch_add. No mutex needed; relaxed ordering suffices
     * because the only contended write is the counter itself. */
    int         next_idx;
} dom_parallel_ctx_t;

static void *dom_parallel_worker(void *arg) {
    dom_parallel_ctx_t *ctx = (dom_parallel_ctx_t *)arg;
    while (1) {
        int i = __atomic_fetch_add(&ctx->next_idx, 1, __ATOMIC_RELAXED);
        if (i >= (int)ctx->n_docs) return NULL;
        dom_parse_eager_nocache(ctx->docs[i]);
    }
}

typedef struct {
    dom_parallel_ctx_t *ctx;
    int                  n_threads;
} dom_parallel_run_arg_t;

static void *dom_parallel_run(void *arg) {
    dom_parallel_run_arg_t *ra = (dom_parallel_run_arg_t *)arg;
    int nt = ra->n_threads;
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)nt);
    int spawned = 0;
    for (int i = 0; i < nt; i++) {
        if (pthread_create(&threads[i], NULL, dom_parallel_worker, ra->ctx) == 0) {
            spawned++;
        }
    }
    /* If thread creation partially failed, drain the queue on the
     * caller thread so the rest still parses (no work is lost). */
    if (spawned < nt) dom_parallel_worker(ra->ctx);
    for (int i = 0; i < spawned; i++) pthread_join(threads[i], NULL);
    free(threads);
    return NULL;
}

static VALUE dom_parallel_parse(VALUE klass, VALUE htmls_v, VALUE n_threads_v) {
    Check_Type(htmls_v, T_ARRAY);
    long n = RARRAY_LEN(htmls_v);
    if (n == 0) return rb_ary_new();

    int n_threads = NUM2INT(n_threads_v);
    if (n_threads < 1) n_threads = 1;
    if (n_threads > (int)n) n_threads = (int)n;

    /* Allocate shell docs and pin the html VALUEs under GVL. */
    dom_doc_t **docs = (dom_doc_t **)calloc((size_t)n, sizeof(dom_doc_t *));
    VALUE *html_strs = (VALUE *)calloc((size_t)n, sizeof(VALUE));
    /* Keep references on the Ruby stack via an Array so GC doesn't
     * sweep the htmls mid-parse. The ARRAY entry pins each VALUE. */
    VALUE pin_array = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE s = rb_ary_entry(htmls_v, i);
        if (!RB_TYPE_P(s, T_STRING)) {
            free(docs); free(html_strs);
            rb_raise(rb_eTypeError, "parallel_parse: element %ld is not a String", i);
        }
        VALUE owned = rb_str_dup(s);
        rb_obj_freeze(owned);
        html_strs[i] = owned;
        rb_ary_push(pin_array, owned);
        dom_doc_t *d = dom_doc_alloc();
        d->html_str_value = owned;
        d->html_buf = RSTRING_PTR(owned);
        d->buf_ptrs[0] = d->html_buf;
        d->buf_strs[0] = owned;
        d->html_len = (size_t)RSTRING_LEN(owned);
        docs[i] = d;
    }

    dom_parallel_ctx_t ctx;
    ctx.docs = docs;
    ctx.n_docs = (size_t)n;
    ctx.next_idx = 0;

    dom_parallel_run_arg_t ra;
    ra.ctx = &ctx;
    ra.n_threads = n_threads;

    /* Drop the GVL for the duration of parse + index build. Workers
     * run on real OS threads; the caller's Ruby thread blocks here
     * (parking the VM) until the join returns. */
    rb_thread_call_without_gvl(dom_parallel_run, &ra, NULL, NULL);

    /* Re-acquired GVL — safe to allocate Ruby objects again. */
    VALUE result = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE wrap = TypedData_Wrap_Struct(klass, &dom_doc_data_type, docs[i]);
        rb_ary_push(result, wrap);
    }
    free(docs);
    free(html_strs);
    /* pin_array can be released now — wrapped Documents hold their own
     * VALUE refs via d->html_str_value (marked through the typeddata mark
     * callback). */
    RB_GC_GUARD(pin_array);
    return result;
}

/* instance methods */

static VALUE dom_size(VALUE self) {
    return ULONG2NUM(get_dom(self)->n_nodes);
}

static VALUE dom_html(VALUE self) {
    dom_doc_t *d = get_dom_raw(self);
    /* Zero-copy: hand back the frozen Ruby String we already hold. */
    return d->html_str_value;
}

/* return first real <html> element id, or first element child of root */
static VALUE dom_root_id(VALUE self) {
    dom_doc_t *d = get_dom(self);
    uint32_t c = d->nodes[0].first_child;
    while (c != DOM_NIL) {
        if (d->nodes[c].type == DOM_TYPE_ELEMENT) {
            if (d->nodes[c].tag_len == 4 && strncasecmp(d->html_buf + d->nodes[c].tag_off, "html", 4) == 0) {
                return UINT2NUM(c);
            }
        }
        c = d->nodes[c].next_sibling;
    }
    /* fall back to first element child */
    c = d->nodes[0].first_child;
    while (c != DOM_NIL) {
        if (d->nodes[c].type == DOM_TYPE_ELEMENT) return UINT2NUM(c);
        c = d->nodes[c].next_sibling;
    }
    return UINT2NUM(0);
}

#define VALIDATE_ID(d, idn)                                        \
    do {                                                           \
        if ((idn) >= (d)->n_nodes) rb_raise(rb_eIndexError, "node id out of range"); \
    } while (0)

static VALUE dom_node_type(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    return INT2NUM(d->nodes[i].type);
}

static VALUE dom_node_name(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    dom_node_t *n = &d->nodes[i];
    const char *p; size_t l;
    if (n->type == DOM_TYPE_ELEMENT) {
        p = NODE_BUF(d, n) + n->tag_off; l = n->tag_len;
    } else if (n->type == DOM_TYPE_TEXT) {
        return make_utf8_str_cstr("#text");
    } else if (n->type == DOM_TYPE_COMMENT) {
        return make_utf8_str_cstr("#comment");
    } else {
        return make_utf8_str_cstr("#document");
    }
    char *buf = ALLOCA_N(char, l);
    for (size_t k = 0; k < l; k++) buf[k] = (char)ascii_lower_c((unsigned char)p[k]);
    VALUE s = rb_str_new(buf, (long)l);
    rb_enc_associate(s, enc_utf8);
    return s;
}

static VALUE dom_node_attr(VALUE self, VALUE id, VALUE name) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    dom_node_t *n = &d->nodes[i];
    if (n->type != DOM_TYPE_ELEMENT || n->attr_count == 0) return Qnil;
    Check_Type(name, T_STRING);
    const char *np = RSTRING_PTR(name);
    long nl = RSTRING_LEN(name);
    for (uint32_t k = 0; k < n->attr_count; k++) {
        dom_attr_t *a = &d->attrs[n->attr_first + k];
        if ((long)a->name_len == nl &&
            strncasecmp(ATTR_BUF(d, a) + a->name_off, np, (size_t)nl) == 0) {
            VALUE v = rb_str_buf_new((long)a->val_len);
            rb_enc_associate(v, enc_utf8);
            append_decoded(ATTR_BUF(d, a) + a->val_off, a->val_len, v);
            return v;
        }
    }
    return Qnil;
}

static VALUE dom_node_attributes(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    dom_node_t *n = &d->nodes[i];
    VALUE h = rb_hash_new();
    if (n->type != DOM_TYPE_ELEMENT) return h;
    for (uint32_t k = 0; k < n->attr_count; k++) {
        dom_attr_t *a = &d->attrs[n->attr_first + k];
        VALUE name = rb_str_new(ATTR_BUF(d, a) + a->name_off, (long)a->name_len);
        rb_enc_associate(name, enc_utf8);
        VALUE val  = rb_str_buf_new((long)a->val_len);
        rb_enc_associate(val,  enc_utf8);
        append_decoded(ATTR_BUF(d, a) + a->val_off, a->val_len, val);
        rb_hash_aset(h, name, val);
    }
    return h;
}

static VALUE dom_node_text(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    VALUE buf = rb_str_buf_new(64);
    rb_enc_associate(buf, enc_utf8);
    append_subtree_text(d, i, buf);
    return buf;
}

static VALUE dom_node_parent(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t p = d->nodes[i].parent;
    return (p == DOM_NIL || p == d->root_id) ? Qnil : UINT2NUM(p);
}

/* Walk forward through the sibling chain, skipping any nodes the user
 * has tombstoned via dom_node_remove. */
static inline uint32_t skip_removed_forward(dom_doc_t *d, uint32_t c) {
    while (c != DOM_NIL && d->nodes[c].type == DOM_TYPE_REMOVED) {
        c = d->nodes[c].next_sibling;
    }
    return c;
}

static inline uint32_t skip_removed_backward(dom_doc_t *d, uint32_t c) {
    while (c != DOM_NIL && d->nodes[c].type == DOM_TYPE_REMOVED) {
        c = d->nodes[c].prev_sibling;
    }
    return c;
}

static VALUE dom_node_first_child(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = skip_removed_forward(d, d->nodes[i].first_child);
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_next_sibling(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = skip_removed_forward(d, d->nodes[i].next_sibling);
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_prev_sibling(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = skip_removed_backward(d, d->nodes[i].prev_sibling);
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_children(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    VALUE ary = rb_ary_new();
    uint32_t c = d->nodes[i].first_child;
    while (c != DOM_NIL) {
        if (d->nodes[c].type != DOM_TYPE_REMOVED) rb_ary_push(ary, UINT2NUM(c));
        c = d->nodes[c].next_sibling;
    }
    return ary;
}

static VALUE dom_node_element_children(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    VALUE ary = rb_ary_new();
    uint32_t c = d->nodes[i].first_child;
    while (c != DOM_NIL) {
        if (d->nodes[c].type == DOM_TYPE_ELEMENT) rb_ary_push(ary, UINT2NUM(c));
        c = d->nodes[c].next_sibling;
    }
    return ary;
}

/* Mutate the arena to detach a node. Update parent's first/last child
 * + the surrounding siblings' next/prev pointers, then mark the node
 * with the REMOVED type so every read path (matcher, child/sibling
 * walks) treats it as gone. The arena slot itself is left allocated —
 * orphaned nodes leak memory until the document is GC'd, but the
 * tradeoff is `node.remove` now operates entirely in native code
 * without falling back to a Ruby Dom view (which the path-locator
 * sometimes can't pin down on parser-divergent HTML). */
static VALUE dom_node_remove(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    dom_node_t *n = &d->nodes[i];
    if (n->type == DOM_TYPE_REMOVED) return Qnil;
    uint32_t p = n->parent;
    uint32_t prev = n->prev_sibling;
    uint32_t next = n->next_sibling;
    if (prev != DOM_NIL) d->nodes[prev].next_sibling = next;
    if (next != DOM_NIL) d->nodes[next].prev_sibling = prev;
    if (p != DOM_NIL) {
        if (d->nodes[p].first_child == i) d->nodes[p].first_child = next;
        if (d->nodes[p].last_child  == i) d->nodes[p].last_child  = prev;
    }
    n->type = DOM_TYPE_REMOVED;
    n->parent = DOM_NIL;
    d->has_removed = 1;
    /* Any cached (selector, scope_id) → ids hits would now be stale
     * for THIS Document — disable the shared memo so subsequent at_css
     * calls re-evaluate. Other Documents sharing the slot still see
     * the cache as long as they don't mutate. */
    d->cache_disabled = 1;
    return Qnil;
}

/* Helper used by the structural mutation path: walk the fragment
 * arena's index entries (class/id/tag) and add corresponding entries
 * to the main doc's indexes, with the remapped id. */
static void add_node_to_indexes(dom_doc_t *d, uint32_t new_id) {
    dom_node_t *n = &d->nodes[new_id];
    if (n->type != DOM_TYPE_ELEMENT) return;
    if (n->tag_len > 0) {
        const char *p = NODE_BUF(d, n) + n->tag_off;
        uint32_t h = fnv1a_ci(p, n->tag_len);
        dom_index_entry_t *e = dom_index_get_or_create(&d->tag_idx, p, n->tag_len, h);
        dom_index_push(e, new_id);
    }
    if (n->class_off != DOM_NIL && n->class_len > 0) {
        const char *val = NODE_BUF(d, n) + n->class_off;
        size_t vl = n->class_len;
        size_t s = 0;
        for (size_t k = 0; k <= vl; k++) {
            if (k == vl || is_ws_byte((unsigned char)val[k])) {
                if (k > s) {
                    uint32_t h = fnv1a_ci(val + s, k - s);
                    dom_index_entry_t *e = dom_index_get_or_create(
                        &d->class_idx, val + s, k - s, h);
                    dom_index_push(e, new_id);
                }
                s = k + 1;
            }
        }
    }
    if (n->id_off != DOM_NIL && n->id_len > 0) {
        const char *val = NODE_BUF(d, n) + n->id_off;
        uint32_t h = fnv1a_ci(val, n->id_len);
        dom_index_entry_t *e = dom_index_get_or_create(&d->id_idx, val, n->id_len, h);
        dom_index_push(e, new_id);
    }
}

/* Replace the children of a native node with a fresh fragment parsed
 * from `html_v`. Stays entirely in C — no Ruby Dom round-trip — so
 * downstream selector queries on the document can keep hitting the
 * native arena. The fragment HTML's bytes get pinned in a new buf
 * slot on the document; new node offsets reference that slot via
 * buf_id. Existing children of the target are tombstoned.
 *
 * The fragment nodes get appended to the arena at high ids, so the
 * pre-order dfs_in/dfs_out range encoding no longer holds for the
 * target's subtree — the document flips d->tree_dirty, which the
 * matcher honours by falling back to parent-walk descendant checks
 * (correct for any tree shape, slightly slower than the range bypass).
 */
static VALUE dom_node_set_inner_html(VALUE self, VALUE id_v, VALUE html_v) {
    dom_doc_t *d = get_dom(self);
    Check_Type(html_v, T_STRING);
    uint32_t target_id = NUM2UINT(id_v);
    VALIDATE_ID(d, target_id);
    if (d->nodes[target_id].type != DOM_TYPE_ELEMENT) {
        rb_raise(rb_eArgError, "inner_html= requires an element node");
    }
    if (d->n_bufs >= DOM_MAX_BUFS) {
        /* Cap reached; bail to Ruby fallback. */
        return Qfalse;
    }

    /* Parse the fragment in a temporary doc. */
    dom_doc_t *frag = dom_doc_alloc();
    VALUE owned = rb_str_dup(html_v);
    rb_obj_freeze(owned);
    frag->html_str_value = owned;
    frag->html_buf = RSTRING_PTR(owned);
    frag->html_len = (size_t)RSTRING_LEN(owned);
    frag->buf_ptrs[0] = frag->html_buf;
    frag->buf_strs[0] = owned;
    frag->parsed = 1;
    dom_parse(frag);
    compute_dfs_out(frag);

    /* Reserve a buf slot on the main doc that points at the fragment's
     * bytes. The Ruby String is pinned via buf_strs so the bytes
     * outlive the frag dom_doc_t. */
    uint8_t new_buf_id = (uint8_t)d->n_bufs;
    d->buf_ptrs[new_buf_id] = frag->html_buf;
    d->buf_strs[new_buf_id] = owned;
    d->n_bufs++;

    /* Tombstone existing children of target. */
    uint32_t old_c = d->nodes[target_id].first_child;
    while (old_c != DOM_NIL) {
        uint32_t next = d->nodes[old_c].next_sibling;
        d->nodes[old_c].type = DOM_TYPE_REMOVED;
        d->nodes[old_c].parent = DOM_NIL;
        d->nodes[old_c].next_sibling = DOM_NIL;
        d->nodes[old_c].prev_sibling = DOM_NIL;
        old_c = next;
    }
    d->has_removed = 1;
    d->nodes[target_id].first_child = DOM_NIL;
    d->nodes[target_id].last_child  = DOM_NIL;

    /* Map fragment node ids → new main ids. fragment id 0 is the doc
     * root — it maps to target_id. All other fragment nodes get
     * freshly-allocated ids on the main doc. */
    uint32_t *idmap = (uint32_t *)malloc(sizeof(uint32_t) * frag->n_nodes);
    idmap[0] = target_id;
    for (uint32_t i = 1; i < frag->n_nodes; i++) {
        idmap[i] = dom_alloc_node(d);
    }

    /* Copy fragment's attribute blob into the main attrs vec. */
    uint32_t attr_offset = 0;
    if (frag->n_attrs > 0) {
        attr_offset = dom_alloc_attrs(d, (uint32_t)frag->n_attrs);
        memcpy(d->attrs + attr_offset, frag->attrs,
               sizeof(dom_attr_t) * frag->n_attrs);
        /* Re-base buf_id on every copied attr — they point at the new
         * buf slot, not the original 0. */
        for (size_t a = 0; a < frag->n_attrs; a++) {
            d->attrs[attr_offset + a].buf_id = new_buf_id;
        }
    }

    /* Copy fragment node data + remap pointer fields + relink to
     * main arena (parent of fragment-root-children becomes target_id). */
    for (uint32_t i = 1; i < frag->n_nodes; i++) {
        uint32_t new_id = idmap[i];
        dom_node_t *src = &frag->nodes[i];
        dom_node_t *dst = &d->nodes[new_id];
        *dst = *src;
        dst->buf_id = new_buf_id;
        dst->parent       = (src->parent == 0) ? target_id : idmap[src->parent];
        dst->first_child  = (src->first_child  == DOM_NIL) ? DOM_NIL : idmap[src->first_child];
        dst->last_child   = (src->last_child   == DOM_NIL) ? DOM_NIL : idmap[src->last_child];
        dst->next_sibling = (src->next_sibling == DOM_NIL) ? DOM_NIL : idmap[src->next_sibling];
        dst->prev_sibling = (src->prev_sibling == DOM_NIL) ? DOM_NIL : idmap[src->prev_sibling];
        if (src->type == DOM_TYPE_ELEMENT && src->attr_count > 0 && src->attr_first != DOM_NIL) {
            dst->attr_first = attr_offset + src->attr_first;
        }
    }

    /* Hook the fragment's root children up under the target. */
    uint32_t f_first = frag->nodes[0].first_child;
    uint32_t f_last  = frag->nodes[0].last_child;
    if (f_first != DOM_NIL) {
        d->nodes[target_id].first_child = idmap[f_first];
        d->nodes[target_id].last_child  = idmap[f_last];
    }

    /* Add new element nodes to the structural indexes. */
    for (uint32_t i = 1; i < frag->n_nodes; i++) {
        add_node_to_indexes(d, idmap[i]);
    }

    /* dfs_out is no longer a valid range encoding for the target's
     * subtree (new ids are appended at the tail of the arena). Mark
     * the tree dirty so the matcher takes the parent-walk fallback. */
    d->tree_dirty = 1;
    compute_dfs_out(d);          /* still update so descendant scans
                                  * stay coherent for sibling subtrees */
    compute_position_indices(d);
    compute_ancestor_blooms(d);

    /* Invalidate the per-document result cache; any (selector, scope)
     * entries that were computed before this mutation may now be
     * stale. */
    d->cache_disabled = 1;

    /* Reset cached_index pointers — they reference the OLD index
     * state. (Actually the cached_index is per c_simple_atom, so this
     * is implicitly fresh on next query.) */

    /* Reset the simple_atom cached_index hints stored on plans —
     * those reference the OLD index lists by pointer; subsequent
     * queries see the augmented index entries. The cache lives on
     * c_simple_atom which is alloca'd per query, so it's already
     * fresh next call. */

    dom_doc_free(frag);
    free(idmap);
    return Qtrue;
}

static VALUE dom_node_is_element(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    return d->nodes[i].type == DOM_TYPE_ELEMENT ? Qtrue : Qfalse;
}

static VALUE dom_node_classes(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    dom_node_t *n = &d->nodes[i];
    VALUE ary = rb_ary_new();
    if (n->type != DOM_TYPE_ELEMENT) return ary;
    for (uint32_t k = 0; k < n->attr_count; k++) {
        dom_attr_t *a = &d->attrs[n->attr_first + k];
        if (a->name_len == 5 && strncasecmp(ATTR_BUF(d, a) + a->name_off, "class", 5) == 0) {
            const char *vp = ATTR_BUF(d, a) + a->val_off;
            size_t vl = a->val_len;
            size_t s = 0;
            while (s < vl) {
                while (s < vl && is_ws_byte((unsigned char)vp[s])) s++;
                size_t e = s;
                while (e < vl && !is_ws_byte((unsigned char)vp[e])) e++;
                if (e > s) {
                    VALUE c = rb_str_new(vp + s, (long)(e - s));
                    rb_enc_associate(c, enc_utf8);
                    rb_ary_push(ary, c);
                }
                s = e;
            }
            return ary;
        }
    }
    return ary;
}

static VALUE dom_class_index_size(VALUE self) {
    dom_doc_t *d = get_dom(self);
    return ULONG2NUM(d->class_idx.count);
}

static VALUE dom_class_index_keys(VALUE self) {
    dom_doc_t *d = get_dom(self);
    VALUE ary = rb_ary_new();
    for (size_t i = 0; i < d->class_idx.cap; i++) {
        dom_index_entry_t *e = &d->class_idx.buckets[i];
        if (!e->used) continue;
        VALUE s = rb_str_new(e->key_ptr, (long)e->key_len);
        rb_enc_associate(s, enc_utf8);
        VALUE pair = rb_ary_new();
        rb_ary_push(pair, s);
        rb_ary_push(pair, ULONG2NUM(e->count));
        rb_ary_push(ary, pair);
    }
    return ary;
}

/* ---- selector matching ------------------------------------------- */

/* sel atom layout (decoded from a Ruby selector string by the Ruby side):
 *
 *   sel = [tag_or_nil, classes, id_or_nil, attrs]
 *   attrs = [[name, op, val], ...]
 *
 * compiled chain: [atom, atom, ...]
 *
 * For efficiency we pre-resolve string fields once into a small C-side
 * representation, then run the matcher.
 */

#define C_MAX_CLASSES 8
#define C_MAX_ATTRS   8

/* Pseudo-class bitmap. Mirrored in lib/scrapetor/native.rb's
 * NATIVE_PSEUDO_FLAGS so the Ruby compiler can emit the same bits
 * we read here. */
#define C_PS_FIRST_CHILD       (1u << 0)
#define C_PS_LAST_CHILD        (1u << 1)
#define C_PS_ONLY_CHILD        (1u << 2)
#define C_PS_FIRST_OF_TYPE     (1u << 3)
#define C_PS_LAST_OF_TYPE      (1u << 4)
#define C_PS_ONLY_OF_TYPE      (1u << 5)
#define C_PS_EMPTY             (1u << 6)
#define C_PS_ROOT              (1u << 7)
#define C_PS_CHECKED           (1u << 8)
#define C_PS_DISABLED          (1u << 9)
#define C_PS_ENABLED           (1u << 10)
#define C_PS_REQUIRED          (1u << 11)
#define C_PS_OPTIONAL          (1u << 12)
#define C_PS_READ_ONLY         (1u << 13)
#define C_PS_READ_WRITE        (1u << 14)
#define C_PS_ANY_LINK          (1u << 15)
#define C_PS_NTH_CHILD         (1u << 16)
#define C_PS_NTH_LAST_CHILD    (1u << 17)
#define C_PS_NTH_OF_TYPE       (1u << 18)
#define C_PS_NTH_LAST_OF_TYPE  (1u << 19)
#define C_PS_NOT               (1u << 20)
#define C_PS_IS                (1u << 21)
#define C_PS_HAS               (1u << 22)
#define C_PS_SCOPE             (1u << 23)
/* `:not(:has(X, Y))` collapses into a single bit on the outer atom +
 * the inner simple atoms stored in the not_has_inner pool. Without
 * this, the recursive form (a `:not` whose inner is itself a `:has`)
 * forces the whole compile to the Ruby Dom fallback path — and that
 * path dominates parse time on scrape workloads that lean on the
 * pattern (a single page can hit it on every iteration of a result
 * list). */
#define C_PS_NOT_HAS           (1u << 24)
/* `:has(> X)` — direct-child variant of :has. The Ruby compiler
 * desugars the leading `>` into `:scope > X` which has two atoms,
 * meaning the generic `has_inner` (single-atom) path can't accept it.
 * Lift the inner to a dedicated has_child_inner pool and check just
 * the direct element children of the candidate. */
#define C_PS_HAS_CHILD         (1u << 25)
/* `:not(:has(> X))` — same shape inverted. Same rationale as
 * C_PS_NOT_HAS, applied to the child-only :has form. */
#define C_PS_NOT_HAS_CHILD     (1u << 26)
/* `:has(X Y)` — :has with a multi-atom chain inside (descendant /
 * child combinators between simple atoms). Same idea as C_PS_HAS but
 * the inner is a chain to verify, not a single simple atom. */
#define C_PS_HAS_CHAIN         (1u << 27)
/* `:has(> ::text)` / `:has(::text)` — element has at least one direct
 * text-node child. Used by parsers that gate on "this
 * heading has its own inline text rather than just nested elements".
 * Implemented as a one-pass scan of the immediate children. */
#define C_PS_HAS_TEXT_CHILD    (1u << 28)
/* `:not(:has(X Y))` — :not(:has) with a chain inner. Symmetric to
 * C_PS_HAS_CHAIN but inverted: the candidate matches when no descendant
 * matches the chain. Stored alongside has_chain_inner but evaluated
 * with the negated check. */
#define C_PS_NOT_HAS_CHAIN     (1u << 29)
/* `:has(+ X)` — the next element sibling of scope matches X.
 * `:has(~ X)` — some later element sibling of scope matches X. The
 * inner simple atoms are stored in has_inner; the bit selects the
 * walk direction. */
#define C_PS_HAS_NEXT_SIB      (1u << 30)
#define C_PS_HAS_LATER_SIB     (1u << 31)

typedef struct {
    const char *name;
    size_t      len;
    int         op;     /* 0 exists, 1 eq, 2 prefix, 3 suffix, 4 contains, 5 word, 6 dash */
    const char *val;
    size_t      vlen;
    int         ci;     /* 1 = case-insensitive (CSS L4 `[a=b i]`) */
} c_attr_m;

/* "Simple" atom — used as the inner selector for :not / :is / :has.
 * Carries the same matchable surface as c_atom (tag/class/id/attrs/
 * positional+boolean pseudos) but no combinator, plus one level of
 * recursive inner pools so `:has(.x:not(.y))` etc. compile natively.
 * The inner pools point into the same shared simple-atom pool — the
 * inner-of-inner atoms must themselves be leaves (no further pools).
 */
typedef struct c_simple_atom_s c_simple_atom;
struct c_simple_atom_s {
    const char *tag;     size_t tag_len;
    /* Interned id from dom_tag_table — 0 if the selector's tag isn't a
     * known HTML/SVG name (custom elements fall through to strncasecmp). */
    uint16_t    tag_id;
    const char *id;      size_t id_len;
    const char *classes[C_MAX_CLASSES];
    size_t      class_lens[C_MAX_CLASSES];
    int         n_classes;
    c_attr_m    attrs[C_MAX_ATTRS];
    int         n_attrs;
    uint32_t    pseudo_flags;   /* positional + boolean pseudos only */
    int         nth_a, nth_b;
    int         nth_type_a, nth_type_b;
    /* Per-query cache of the narrowest structural-index entry for this
     * atom. NULL means "not yet resolved"; (void *)1 means "resolved,
     * no index available" — set in has_descendant_via_index so each
     * `:has(...)` evaluation does one O(1) hash lookup per query
     * instead of one per candidate. Reset to NULL by build_simple_atom's
     * memset, so a fresh dom_run_chain call starts with an empty cache. */
    void *cached_index;
    /* For atoms stored in a has_chain pool: the combinator linking the
     * previous chain atom to this one. 0=none (first atom), 1=descendant,
     * 2=child. Lives on the atom itself so the chain doesn't need a
     * separate parallel array (which would need its own pool lifetime). */
    uint8_t chain_combo;
    /* Recursive constraints lifted from `:not(...)` / `:has(...)` /
     * `:not(:has(...))` pseudos on the atom. The atoms in these inner
     * pools live in the same shared simple-atom pool as the parent;
     * they themselves must have no further recursion (the Ruby
     * compiler enforces this by only emitting one level). */
    const c_simple_atom *inner_not;
    int n_inner_not;
    const c_simple_atom *inner_has;
    int n_inner_has;
    const c_simple_atom *inner_not_has;
    int n_inner_not_has;
    /* `:has(X Y, A B, ...)` constraint on the simple atom itself —
     * e.g. when this atom lives inside an outer :has chain and itself
     * carries a multi-atom :has. Chain atoms are leaves (no further
     * recursion). Capped at 4 chains × 255 atoms each. */
    const c_simple_atom *inner_has_chain;
    int n_inner_has_chain;
    uint8_t inner_has_chain_count;
    uint8_t inner_has_chain_lens[4];
};

typedef struct {
    const char *tag;     size_t tag_len;
    uint16_t    tag_id;         /* mirror of c_simple_atom::tag_id */
    /* Bloom signature of THIS atom's tag/classes/id, hashed under
     * dom_bloom_bits_ci. Used by the chain dispatcher to construct a
     * `required_ancestor_bloom` mask from the non-rightmost atoms and
     * fast-reject candidates whose ancestor_bloom doesn't cover the
     * required bits. */
    uint64_t    self_bloom;
    const char *id;      size_t id_len;
    const char *classes[C_MAX_CLASSES];
    size_t      class_lens[C_MAX_CLASSES];
    int         n_classes;
    c_attr_m    attrs[C_MAX_ATTRS];
    int         n_attrs;
    int         combinator;     /* 0 none, 1 descendant, 2 child */
    /* Pseudo-class data. pseudo_flags is a bitmap of which checks are
     * active; the nth_* fields carry the formula coefficients for
     * :nth-child / :nth-of-type variants. Inner-selector pointers
     * (allocated in the same alloca'd pool as the atoms) drive
     * :not / :is / :has matching without growing the atom struct. */
    uint32_t    pseudo_flags;
    int         nth_a, nth_b;
    int         nth_type_a, nth_type_b;
    const c_simple_atom *not_inner;
    int         n_not_inner;
    const c_simple_atom *is_inner;
    int         n_is_inner;
    const c_simple_atom *has_inner;
    int         n_has_inner;
    /* Inner atoms for `:not(:has(X, Y))`. Same shape as has_inner, but
     * evaluation is inverted: the candidate matches when no descendant
     * matches any of these. */
    const c_simple_atom *not_has_inner;
    int         n_not_has_inner;
    /* `:has(> X)` and `:not(:has(> X))` — direct-child variants. */
    const c_simple_atom *has_child_inner;
    int         n_has_child_inner;
    const c_simple_atom *not_has_child_inner;
    int         n_not_has_child_inner;
    /* `:has(X Y, A B, ...)` — N chains of simple atoms. Atoms for all
     * chains live concatenated in has_chain_inner; the length of each
     * chain is at has_chain_lens[k]. C_PS_HAS_CHAIN matches if ANY of
     * the chains has a descendant match. Capped at 8 chains; selectors
     * with more alternatives fall back to the Ruby path. The same
     * shape mirrors not_has_chain_*. */
    const c_simple_atom *has_chain_inner;
    int         has_chain_len;          /* total atoms (sum across chains) */
    uint8_t     has_chain_count;
    uint8_t     has_chain_lens[8];
    const c_simple_atom *not_has_chain_inner;
    int         not_has_chain_len;
    uint8_t     not_has_chain_count;
    uint8_t     not_has_chain_lens[8];
} c_atom;

static int parse_attr_op(const char *p, long l) {
    if (l == 1 && p[0] == '=') return 1;
    if (l == 2 && p[1] == '=') {
        switch (p[0]) {
        case '*': return 4;
        case '^': return 2;
        case '$': return 3;
        case '~': return 5;
        case '|': return 6;
        }
    }
    return -1;
}

static int build_simple_atom_full(VALUE sel_v, c_simple_atom *out,
                                  c_simple_atom *pool, long *pool_used,
                                  int recursion_depth);

/* Populate a c_simple_atom from a Ruby `[tag, classes, id, attrs]`
 * array. Used both directly (for inner :not/:is/:has atoms) and
 * indirectly (build_atom copies the simple part the same way). */
static int build_simple_atom(VALUE sel_v, c_simple_atom *out) {
    return build_simple_atom_full(sel_v, out, NULL, NULL, 0);
}

static int build_simple_atom_full(VALUE sel_v, c_simple_atom *out,
                                  c_simple_atom *pool, long *pool_used,
                                  int recursion_depth) {
    memset(out, 0, sizeof(*out));
    if (!RB_TYPE_P(sel_v, T_ARRAY) || RARRAY_LEN(sel_v) < 4) return 0;

    VALUE tag = rb_ary_entry(sel_v, 0);
    if (!NIL_P(tag)) {
        if (!RB_TYPE_P(tag, T_STRING)) return 0;
        out->tag = RSTRING_PTR(tag); out->tag_len = (size_t)RSTRING_LEN(tag);
        out->tag_id = dom_intern_tag(out->tag, out->tag_len);
    }

    VALUE classes = rb_ary_entry(sel_v, 1);
    if (!RB_TYPE_P(classes, T_ARRAY)) return 0;
    long nc = RARRAY_LEN(classes);
    if (nc > C_MAX_CLASSES) return 0;
    for (long i = 0; i < nc; i++) {
        VALUE c = rb_ary_entry(classes, i);
        if (!RB_TYPE_P(c, T_STRING)) return 0;
        out->classes[i] = RSTRING_PTR(c);
        out->class_lens[i] = (size_t)RSTRING_LEN(c);
    }
    out->n_classes = (int)nc;

    VALUE id = rb_ary_entry(sel_v, 2);
    if (!NIL_P(id)) {
        if (!RB_TYPE_P(id, T_STRING)) return 0;
        out->id = RSTRING_PTR(id); out->id_len = (size_t)RSTRING_LEN(id);
    }

    VALUE attrs = rb_ary_entry(sel_v, 3);
    if (!RB_TYPE_P(attrs, T_ARRAY)) return 0;
    long na = RARRAY_LEN(attrs);
    if (na > C_MAX_ATTRS) return 0;
    for (long i = 0; i < na; i++) {
        VALUE a = rb_ary_entry(attrs, i);
        if (!RB_TYPE_P(a, T_ARRAY) || RARRAY_LEN(a) < 3) return 0;
        VALUE n = rb_ary_entry(a, 0);
        VALUE o = rb_ary_entry(a, 1);
        VALUE v = rb_ary_entry(a, 2);
        VALUE ci = (RARRAY_LEN(a) >= 4) ? rb_ary_entry(a, 3) : Qfalse;
        if (!RB_TYPE_P(n, T_STRING)) return 0;
        out->attrs[i].name = RSTRING_PTR(n);
        out->attrs[i].len  = (size_t)RSTRING_LEN(n);
        out->attrs[i].ci   = RTEST(ci) ? 1 : 0;
        if (NIL_P(o)) {
            out->attrs[i].op = 0;
        } else {
            if (!RB_TYPE_P(o, T_STRING)) return 0;
            int op = parse_attr_op(RSTRING_PTR(o), RSTRING_LEN(o));
            if (op < 0) return 0;
            out->attrs[i].op = op;
            if (!RB_TYPE_P(v, T_STRING)) return 0;
            out->attrs[i].val = RSTRING_PTR(v);
            out->attrs[i].vlen = (size_t)RSTRING_LEN(v);
        }
    }
    out->n_attrs = (int)na;

    /* Optional pseudo data on inner atoms. Layout:
     *   [0]      flags bitmap
     *   [1..4]   nth coefficients
     *   [5]      inner_not pool (optional — one level of recursion)
     *   [6]      inner_has pool (optional)
     *   [7]      inner_not_has pool (optional)
     */
    if (RARRAY_LEN(sel_v) >= 5) {
        VALUE pseudo = rb_ary_entry(sel_v, 4);
        if (!NIL_P(pseudo) && RB_TYPE_P(pseudo, T_ARRAY) && RARRAY_LEN(pseudo) >= 5) {
            VALUE flags_v = rb_ary_entry(pseudo, 0);
            if (RB_INTEGER_TYPE_P(flags_v)) {
                out->pseudo_flags = (uint32_t)NUM2UINT(flags_v);
                out->nth_a      = NUM2INT(rb_ary_entry(pseudo, 1));
                out->nth_b      = NUM2INT(rb_ary_entry(pseudo, 2));
                out->nth_type_a = NUM2INT(rb_ary_entry(pseudo, 3));
                out->nth_type_b = NUM2INT(rb_ary_entry(pseudo, 4));
            }
            /* Inner pools — only at top level of recursion. Reserve
             * the slots in the shared pool BEFORE recursing so nested
             * allocations from the recursive call start past us. */
            if (pool && recursion_depth == 0 && RARRAY_LEN(pseudo) >= 6) {
                VALUE inner_not = rb_ary_entry(pseudo, 5);
                if (RB_TYPE_P(inner_not, T_ARRAY) && RARRAY_LEN(inner_not) > 0) {
                    long m = RARRAY_LEN(inner_not);
                    c_simple_atom *base = pool + *pool_used;
                    *pool_used += m;
                    for (long i = 0; i < m; i++) {
                        if (!build_simple_atom_full(rb_ary_entry(inner_not, i), &base[i],
                                                    pool, pool_used, recursion_depth + 1)) return 0;
                    }
                    out->inner_not = base;
                    out->n_inner_not = (int)m;
                }
            }
            if (pool && recursion_depth == 0 && RARRAY_LEN(pseudo) >= 7) {
                VALUE inner_has = rb_ary_entry(pseudo, 6);
                if (RB_TYPE_P(inner_has, T_ARRAY) && RARRAY_LEN(inner_has) > 0) {
                    long m = RARRAY_LEN(inner_has);
                    c_simple_atom *base = pool + *pool_used;
                    *pool_used += m;
                    for (long i = 0; i < m; i++) {
                        if (!build_simple_atom_full(rb_ary_entry(inner_has, i), &base[i],
                                                    pool, pool_used, recursion_depth + 1)) return 0;
                    }
                    out->inner_has = base;
                    out->n_inner_has = (int)m;
                }
            }
            if (pool && recursion_depth == 0 && RARRAY_LEN(pseudo) >= 8) {
                VALUE inner_nh = rb_ary_entry(pseudo, 7);
                if (RB_TYPE_P(inner_nh, T_ARRAY) && RARRAY_LEN(inner_nh) > 0) {
                    long m = RARRAY_LEN(inner_nh);
                    c_simple_atom *base = pool + *pool_used;
                    *pool_used += m;
                    for (long i = 0; i < m; i++) {
                        if (!build_simple_atom_full(rb_ary_entry(inner_nh, i), &base[i],
                                                    pool, pool_used, recursion_depth + 1)) return 0;
                    }
                    out->inner_not_has = base;
                    out->n_inner_not_has = (int)m;
                }
            }
            /* pseudo[8] — inner has_chain. Format mirrors the outer
             * has_chain at pseudo[11]: array of chains, each chain an
             * array of [atom, combo]. Inner chain atoms are leaves. */
            if (pool && recursion_depth == 0 && RARRAY_LEN(pseudo) >= 9) {
                VALUE chains_v = rb_ary_entry(pseudo, 8);
                if (RB_TYPE_P(chains_v, T_ARRAY) && RARRAY_LEN(chains_v) > 0) {
                    long n_chains = RARRAY_LEN(chains_v);
                    if (n_chains > 4) return 0; /* cap == lens array size */
                    c_simple_atom *base = pool + *pool_used;
                    long written = 0;
                    uint8_t cnt = 0;
                    for (long ci = 0; ci < n_chains; ci++) {
                        VALUE chain = rb_ary_entry(chains_v, ci);
                        if (!RB_TYPE_P(chain, T_ARRAY)) return 0;
                        long clen = RARRAY_LEN(chain);
                        if (clen == 0 || clen > 255) return 0;
                        for (long ai = 0; ai < clen; ai++) {
                            VALUE e = rb_ary_entry(chain, ai);
                            if (!RB_TYPE_P(e, T_ARRAY) || RARRAY_LEN(e) < 1) return 0;
                            VALUE sel = rb_ary_entry(e, 0);
                            *pool_used += 1;
                            if (!build_simple_atom_full(sel, &base[written],
                                pool, pool_used, recursion_depth + 1)) return 0;
                            uint8_t cb = 0;
                            if (RARRAY_LEN(e) >= 2) {
                                VALUE combo = rb_ary_entry(e, 1);
                                if (!NIL_P(combo)) {
                                    if (!RB_TYPE_P(combo, T_STRING)) return 0;
                                    long cl = RSTRING_LEN(combo);
                                    const char *cp = RSTRING_PTR(combo);
                                    if      (cl == 10 && memcmp(cp, "descendant", 10) == 0) cb = 1;
                                    else if (cl == 5  && memcmp(cp, "child", 5) == 0)       cb = 2;
                                    else if (cl == 8  && memcmp(cp, "adjacent", 8) == 0)    cb = 3;
                                    else if (cl == 7  && memcmp(cp, "sibling", 7) == 0)     cb = 4;
                                    else return 0;
                                }
                            }
                            base[written].chain_combo = cb;
                            written++;
                        }
                        out->inner_has_chain_lens[cnt++] = (uint8_t)clen;
                    }
                    out->inner_has_chain = base;
                    out->n_inner_has_chain = (int)written;
                    out->inner_has_chain_count = cnt;
                }
            }
        }
    }
    return 1;
}

/* Pseudo-side fields are populated separately in dom_run_chain (we need
 * a pre-pass over the plan to alloca the inner-atom pool). */
static int build_atom(VALUE sel_v, c_atom *out) {
    memset(out, 0, sizeof(*out));
    /* Reuse the simple-atom path for tag/classes/id/attrs. */
    c_simple_atom tmp;
    if (!build_simple_atom(sel_v, &tmp)) return 0;
    out->tag        = tmp.tag;
    out->tag_len    = tmp.tag_len;
    out->tag_id     = tmp.tag_id;
    /* Aggregate this atom's signature for ancestor-bloom filtering. The
     * bits must match dom_node_self_bloom's byte-level hashing so the
     * mask AND check is meaningful. */
    {
        uint64_t b = 0;
        if (tmp.tag_len > 0) b |= dom_bloom_bits_ci(tmp.tag, tmp.tag_len);
        for (int ci = 0; ci < tmp.n_classes; ci++) {
            b |= dom_bloom_bits_ci(tmp.classes[ci], tmp.class_lens[ci]);
        }
        if (tmp.id_len > 0) b |= dom_bloom_bits_ci(tmp.id, tmp.id_len);
        out->self_bloom = b;
    }
    out->id         = tmp.id;
    out->id_len     = tmp.id_len;
    out->n_classes  = tmp.n_classes;
    memcpy(out->classes,    tmp.classes,    sizeof(tmp.classes));
    memcpy(out->class_lens, tmp.class_lens, sizeof(tmp.class_lens));
    out->n_attrs    = tmp.n_attrs;
    memcpy(out->attrs, tmp.attrs, sizeof(tmp.attrs));
    return 1;
}

/* Count how many c_simple_atoms we need for inner :not/:is/:has
 * selectors across the whole plan. Caller alloca's the pool. */
static long count_inner_atoms(VALUE plan_v) {
    long total = 0;
    long n = RARRAY_LEN(plan_v);
    for (long i = 0; i < n; i++) {
        VALUE entry = rb_ary_entry(plan_v, i);
        if (!RB_TYPE_P(entry, T_ARRAY) || RARRAY_LEN(entry) < 1) continue;
        VALUE sel = rb_ary_entry(entry, 0);
        if (!RB_TYPE_P(sel, T_ARRAY) || RARRAY_LEN(sel) < 5) continue;
        VALUE pseudo = rb_ary_entry(sel, 4);
        if (NIL_P(pseudo) || !RB_TYPE_P(pseudo, T_ARRAY) || RARRAY_LEN(pseudo) < 8) continue;
        long pseudo_len = RARRAY_LEN(pseudo);
        /* Inner pools live at indices 5..n in the plan array:
         *   5 not_inner
         *   6 is_inner
         *   7 has_inner
         *   8 not_has_inner       (optional — older plans stop at 8 entries)
         *   9 has_child_inner     (optional)
         *  10 not_has_child_inner (optional)
         *  11 has_chain_inner     (optional — array of [atom, combo])
         *  12 not_has_chain_inner (optional — same shape as 11)
         */
        long last = pseudo_len - 1;
        if (last > 12) last = 12;
        for (long k = 5; k <= last; k++) {
            VALUE inner = rb_ary_entry(pseudo, k);
            if (!RB_TYPE_P(inner, T_ARRAY)) continue;
            long inner_n = RARRAY_LEN(inner);

            /* has_chain (k=11) / not_has_chain (k=12) emit a list of
             * chains; each chain is itself an array of [atom, combo].
             * Sum the chain lengths so the pool reservation is right. */
            if (k == 11 || k == 12) {
                for (long j = 0; j < inner_n; j++) {
                    VALUE chain = rb_ary_entry(inner, j);
                    if (RB_TYPE_P(chain, T_ARRAY)) total += RARRAY_LEN(chain);
                }
                continue;
            }

            total += inner_n;
            for (long j = 0; j < inner_n; j++) {
                VALUE ie = rb_ary_entry(inner, j);
                VALUE isel = ie;
                if (!RB_TYPE_P(isel, T_ARRAY) || RARRAY_LEN(isel) < 5) continue;
                VALUE ipseudo = rb_ary_entry(isel, 4);
                if (NIL_P(ipseudo) || !RB_TYPE_P(ipseudo, T_ARRAY)) continue;
                long ipl = RARRAY_LEN(ipseudo);
                for (long q = 5; q < ipl && q <= 7; q++) {
                    VALUE ii = rb_ary_entry(ipseudo, q);
                    if (RB_TYPE_P(ii, T_ARRAY)) total += RARRAY_LEN(ii);
                }
                /* Inner has_chain at pseudo[8]: array of chains. */
                if (ipl >= 9) {
                    VALUE chains = rb_ary_entry(ipseudo, 8);
                    if (RB_TYPE_P(chains, T_ARRAY)) {
                        long ncc = RARRAY_LEN(chains);
                        for (long c = 0; c < ncc; c++) {
                            VALUE cc = rb_ary_entry(chains, c);
                            if (RB_TYPE_P(cc, T_ARRAY)) total += RARRAY_LEN(cc);
                        }
                    }
                }
            }
        }
    }
    return total;
}

/* Read pseudo-class data (flags + nth coefficients + inner atom lists)
 * from the Ruby plan entry into the c_atom. `pool` is the alloca'd
 * c_simple_atom buffer; `pool_used` tracks how far we've consumed. */
static int build_atom_pseudos(VALUE sel_v, c_atom *out,
                              c_simple_atom *pool, long *pool_used) {
    if (!RB_TYPE_P(sel_v, T_ARRAY) || RARRAY_LEN(sel_v) < 5) return 1;
    VALUE pseudo = rb_ary_entry(sel_v, 4);
    if (NIL_P(pseudo)) return 1;
    if (!RB_TYPE_P(pseudo, T_ARRAY) || RARRAY_LEN(pseudo) < 8) return 0;

    VALUE flags_v = rb_ary_entry(pseudo, 0);
    if (!RB_INTEGER_TYPE_P(flags_v)) return 0;
    out->pseudo_flags = (uint32_t)NUM2UINT(flags_v);

    out->nth_a      = NUM2INT(rb_ary_entry(pseudo, 1));
    out->nth_b      = NUM2INT(rb_ary_entry(pseudo, 2));
    out->nth_type_a = NUM2INT(rb_ary_entry(pseudo, 3));
    out->nth_type_b = NUM2INT(rb_ary_entry(pseudo, 4));

    /* Allocator helper: reserve `m` slots starting at `*pool_used`,
     * BUMP `*pool_used` immediately so nested allocations performed by
     * build_simple_atom_full (its inner_not / inner_has / inner_not_has
     * pools) start past the reservation instead of overwriting it. */
#define RESERVE_POOL_BASE(_arr_v, _m)                                     \
    c_simple_atom *base = pool + *pool_used;                              \
    *pool_used += (_m);                                                   \
    for (long _i = 0; _i < (_m); _i++) {                                  \
        if (!build_simple_atom_full(rb_ary_entry((_arr_v), _i),           \
                                    &base[_i], pool, pool_used, 0))       \
            return 0;                                                     \
    }

    VALUE not_arr = rb_ary_entry(pseudo, 5);
    if (RB_TYPE_P(not_arr, T_ARRAY) && RARRAY_LEN(not_arr) > 0) {
        long m = RARRAY_LEN(not_arr);
        RESERVE_POOL_BASE(not_arr, m);
        out->not_inner = base;
        out->n_not_inner = (int)m;
    }

    VALUE is_arr = rb_ary_entry(pseudo, 6);
    if (RB_TYPE_P(is_arr, T_ARRAY) && RARRAY_LEN(is_arr) > 0) {
        long m = RARRAY_LEN(is_arr);
        RESERVE_POOL_BASE(is_arr, m);
        out->is_inner = base;
        out->n_is_inner = (int)m;
    }

    VALUE has_arr = rb_ary_entry(pseudo, 7);
    if (RB_TYPE_P(has_arr, T_ARRAY) && RARRAY_LEN(has_arr) > 0) {
        long m = RARRAY_LEN(has_arr);
        RESERVE_POOL_BASE(has_arr, m);
        out->has_inner = base;
        out->n_has_inner = (int)m;
    }

    long pseudo_len = RARRAY_LEN(pseudo);
    if (pseudo_len >= 9) {
        VALUE not_has_arr = rb_ary_entry(pseudo, 8);
        if (RB_TYPE_P(not_has_arr, T_ARRAY) && RARRAY_LEN(not_has_arr) > 0) {
            long m = RARRAY_LEN(not_has_arr);
            RESERVE_POOL_BASE(not_has_arr, m);
            out->not_has_inner = base;
            out->n_not_has_inner = (int)m;
        }
    }
    if (pseudo_len >= 10) {
        VALUE arr = rb_ary_entry(pseudo, 9);
        if (RB_TYPE_P(arr, T_ARRAY) && RARRAY_LEN(arr) > 0) {
            long m = RARRAY_LEN(arr);
            RESERVE_POOL_BASE(arr, m);
            out->has_child_inner = base;
            out->n_has_child_inner = (int)m;
        }
    }
    if (pseudo_len >= 11) {
        VALUE arr = rb_ary_entry(pseudo, 10);
        if (RB_TYPE_P(arr, T_ARRAY) && RARRAY_LEN(arr) > 0) {
            long m = RARRAY_LEN(arr);
            RESERVE_POOL_BASE(arr, m);
            out->not_has_child_inner = base;
            out->n_not_has_child_inner = (int)m;
        }
    }
    /* `:has(X Y, A B, ...)` — multi-chain. pseudo[11] is an array of
     * chains; each chain is an array of [atom, combo] pairs. Atoms
     * across all chains live concatenated in has_chain_inner; per-
     * chain lengths land in has_chain_lens. */
#define READ_CHAIN_POOL(_arr_v, _atoms_ptr, _total_len, _count, _lens_arr)    \
    do {                                                                       \
        long _nchains = RARRAY_LEN(_arr_v);                                    \
        if (_nchains > 8) return 0; /* cap matches has_chain_lens[8] */        \
        c_simple_atom *_base = pool + *pool_used;                              \
        long _written = 0;                                                     \
        uint8_t _cnt = 0;                                                      \
        for (long _ci = 0; _ci < _nchains; _ci++) {                            \
            VALUE _chain = rb_ary_entry((_arr_v), _ci);                        \
            if (!RB_TYPE_P(_chain, T_ARRAY)) return 0;                         \
            long _clen = RARRAY_LEN(_chain);                                   \
            if (_clen > 255) return 0;                                         \
            for (long _ai = 0; _ai < _clen; _ai++) {                           \
                VALUE _e = rb_ary_entry(_chain, _ai);                          \
                if (!RB_TYPE_P(_e, T_ARRAY) || RARRAY_LEN(_e) < 1) return 0;   \
                VALUE _sel = rb_ary_entry(_e, 0);                              \
                *pool_used += 1;                                               \
                if (!build_simple_atom_full(_sel, &_base[_written],            \
                                            pool, pool_used, 0)) return 0;     \
                uint8_t _cb = 0;                                               \
                if (RARRAY_LEN(_e) >= 2) {                                     \
                    VALUE _combo = rb_ary_entry(_e, 1);                        \
                    if (!NIL_P(_combo)) {                                      \
                        if (!RB_TYPE_P(_combo, T_STRING)) return 0;            \
                        long _cl = RSTRING_LEN(_combo);                        \
                        const char *_cp = RSTRING_PTR(_combo);                 \
                        if      (_cl == 10 && memcmp(_cp, "descendant", 10) == 0) _cb = 1; \
                        else if (_cl == 5  && memcmp(_cp, "child", 5) == 0)       _cb = 2; \
                        else if (_cl == 8  && memcmp(_cp, "adjacent", 8) == 0)    _cb = 3; \
                        else if (_cl == 7  && memcmp(_cp, "sibling", 7) == 0)     _cb = 4; \
                        else return 0;                                         \
                    }                                                          \
                }                                                              \
                _base[_written].chain_combo = _cb;                             \
                _written++;                                                    \
            }                                                                  \
            (_lens_arr)[_cnt++] = (uint8_t)_clen;                              \
        }                                                                      \
        (_atoms_ptr) = _base;                                                  \
        (_total_len) = (int)_written;                                          \
        (_count) = _cnt;                                                       \
    } while (0)

    if (pseudo_len >= 12) {
        VALUE arr = rb_ary_entry(pseudo, 11);
        if (RB_TYPE_P(arr, T_ARRAY) && RARRAY_LEN(arr) > 0) {
            READ_CHAIN_POOL(arr, out->has_chain_inner, out->has_chain_len,
                            out->has_chain_count, out->has_chain_lens);
        }
    }
    if (pseudo_len >= 13) {
        VALUE arr = rb_ary_entry(pseudo, 12);
        if (RB_TYPE_P(arr, T_ARRAY) && RARRAY_LEN(arr) > 0) {
            READ_CHAIN_POOL(arr, out->not_has_chain_inner, out->not_has_chain_len,
                            out->not_has_chain_count, out->not_has_chain_lens);
        }
    }
#undef READ_CHAIN_POOL

#undef RESERVE_POOL_BASE
    return 1;
}

/* ---- pseudo-class helpers --------------------------------------- */

static inline uint32_t prev_element_sibling_id(dom_doc_t *d, uint32_t id) {
    uint32_t s = d->nodes[id].prev_sibling;
    while (s != DOM_NIL && d->nodes[s].type != DOM_TYPE_ELEMENT) {
        s = d->nodes[s].prev_sibling;
    }
    return s;
}

static inline uint32_t next_element_sibling_id(dom_doc_t *d, uint32_t id) {
    uint32_t s = d->nodes[id].next_sibling;
    while (s != DOM_NIL && d->nodes[s].type != DOM_TYPE_ELEMENT) {
        s = d->nodes[s].next_sibling;
    }
    return s;
}

/* Returns 1 if there is no preceding element sibling. */
static inline int is_first_element_child(dom_doc_t *d, uint32_t id) {
    return prev_element_sibling_id(d, id) == DOM_NIL;
}

static inline int is_last_element_child(dom_doc_t *d, uint32_t id) {
    return next_element_sibling_id(d, id) == DOM_NIL;
}

/* Walk preceding/following siblings looking for one with the same tag. */
static inline int dom_same_tag(dom_doc_t *d, const dom_node_t *m, const dom_node_t *n) {
    /* Interned-id fast path. Both 0 (= unknown tag) also matches via
     * id equality but that's ambiguous — fall back to byte compare. */
    if (m->tag_id != 0 && n->tag_id != 0) return m->tag_id == n->tag_id;
    if (m->tag_len != n->tag_len) return 0;
    return strncasecmp(NODE_BUF(d, m) + m->tag_off,
                       NODE_BUF(d, n) + n->tag_off, n->tag_len) == 0;
}

static inline int is_first_of_type(dom_doc_t *d, uint32_t id) {
    dom_node_t *n = &d->nodes[id];
    uint32_t s = n->prev_sibling;
    while (s != DOM_NIL) {
        dom_node_t *m = &d->nodes[s];
        if (m->type == DOM_TYPE_ELEMENT && dom_same_tag(d, m, n)) return 0;
        s = m->prev_sibling;
    }
    return 1;
}

static inline int is_last_of_type(dom_doc_t *d, uint32_t id) {
    dom_node_t *n = &d->nodes[id];
    uint32_t s = n->next_sibling;
    while (s != DOM_NIL) {
        dom_node_t *m = &d->nodes[s];
        if (m->type == DOM_TYPE_ELEMENT && dom_same_tag(d, m, n)) return 0;
        s = m->next_sibling;
    }
    return 1;
}

/* 1-based index of `id` within its parent's element children. Cached
 * at parse time so this is a single 32-bit load. */
static inline int element_position_index(dom_doc_t *d, uint32_t id, int reverse, int by_type) {
    dom_node_t *n = &d->nodes[id];
    if (by_type) return (int)(reverse ? n->type_idx_rev  : n->type_idx);
    return       (int)(reverse ? n->child_idx_rev : n->child_idx);
}

static inline int nth_formula_matches(int a, int b, int idx) {
    if (a == 0) return idx == b;
    int diff = idx - b;
    if (a > 0 && diff < 0) return 0;
    if (a < 0 && diff > 0) return 0;
    /* C99: signed integer modulo follows the sign of the dividend. */
    int rem = diff % a;
    return rem == 0;
}

static int truthy_bool_attr(dom_doc_t *d, uint32_t id, const char *name, size_t nlen) {
    dom_node_t *n = &d->nodes[id];
    for (uint32_t k = 0; k < n->attr_count; k++) {
        dom_attr_t *ax = &d->attrs[n->attr_first + k];
        if (ax->name_len == nlen &&
            strncasecmp(ATTR_BUF(d, ax) + ax->name_off, name, nlen) == 0) {
            /* "false" value -> falsy, otherwise truthy. */
            if (ax->val_len == 5 &&
                strncasecmp(ATTR_BUF(d, ax) + ax->val_off, "false", 5) == 0) return 0;
            return 1;
        }
    }
    return 0;
}

/* Forward declarations: simple-atom matcher and atom matcher recurse
 * across :has() / :is() / :not() evaluation. */
static int matches_simple_atom(dom_doc_t *d, uint32_t id, const c_simple_atom *a);
static int has_descendant_chain_match(dom_doc_t *d, uint32_t id,
                                      const c_simple_atom *atoms, int n);
static int has_descendant_matching_simple(dom_doc_t *d, uint32_t id,
                                          const c_simple_atom *atoms, int n);

/* Token-membership test for whitespace-separated class lists. The very
 * common case is "class attribute is exactly the target class" — we
 * short-circuit that with a single memcmp before falling back to the
 * token walk. */
static __attribute__((always_inline)) inline int class_in_attr(const char *attr_val, size_t vlen, const char *cls, size_t clen) {
    if (clen == 0 || vlen < clen) return 0;
    /* Fast path: class attr is exactly the target class. */
    if (vlen == clen && memcmp(attr_val, cls, clen) == 0) return 1;
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && is_ws_byte((unsigned char)attr_val[i])) i++;
        size_t s = i;
        while (i < vlen && !is_ws_byte((unsigned char)attr_val[i])) i++;
        if (i - s == clen && memcmp(attr_val + s, cls, clen) == 0) return 1;
    }
    return 0;
}

/* Case-insensitive variant of class_in_attr. Only ASCII letter-folding,
 * same scope as strncasecmp — sufficient for CSS L4 `[a~=b i]`. */
static __attribute__((always_inline)) inline int class_in_attr_ci(const char *attr_val, size_t vlen, const char *cls, size_t clen) {
    if (clen == 0 || vlen < clen) return 0;
    if (vlen == clen && strncasecmp(attr_val, cls, clen) == 0) return 1;
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && is_ws_byte((unsigned char)attr_val[i])) i++;
        size_t s = i;
        while (i < vlen && !is_ws_byte((unsigned char)attr_val[i])) i++;
        if (i - s == clen && strncasecmp(attr_val + s, cls, clen) == 0) return 1;
    }
    return 0;
}

/* Evaluate a single attribute comparison once the candidate value was
 * found on the element. Returns 1 on match, 0 on miss. Consolidates the
 * switch that used to live in both matches_simple_atom and
 * element_matches_atom; one place to add the case-insensitive `ci`
 * branches. */
static __attribute__((always_inline)) inline int attr_op_match(int op,
                                                               const char *avp, size_t avl,
                                                               const char *vp,  size_t vl,
                                                               int ci) {
    switch (op) {
    case 0: return 1;
    case 1: /* = */
        if (avl != vl) return 0;
        return ci ? (strncasecmp(avp, vp, vl) == 0)
                  : (memcmp(avp, vp, vl) == 0);
    case 2: /* ^= */
        if (avl < vl) return 0;
        return ci ? (strncasecmp(avp, vp, vl) == 0)
                  : (memcmp(avp, vp, vl) == 0);
    case 3: /* $= */
        if (avl < vl) return 0;
        return ci ? (strncasecmp(avp + avl - vl, vp, vl) == 0)
                  : (memcmp(avp + avl - vl, vp, vl) == 0);
    case 4: /* *= */
        if (avl < vl) return 0;
        if (ci) {
            for (size_t k = 0; k + vl <= avl; k++) {
                if (strncasecmp(avp + k, vp, vl) == 0) return 1;
            }
        } else {
            for (size_t k = 0; k + vl <= avl; k++) {
                if (memcmp(avp + k, vp, vl) == 0) return 1;
            }
        }
        return 0;
    case 5: /* ~= */
        return ci ? class_in_attr_ci(avp, avl, vp, vl)
                  : class_in_attr(avp, avl, vp, vl);
    case 6: /* |= */
        if (avl < vl) return 0;
        if (ci ? (strncasecmp(avp, vp, vl) != 0)
               : (memcmp(avp, vp, vl) != 0)) return 0;
        if (avl > vl && avp[vl] != '-') return 0;
        return 1;
    }
    return 0;
}

/* Match the simple (non-pseudo) part of an atom against a node. Pulled
 * out so :not / :is / :has can reuse the same predicate without the
 * recursive pseudo machinery. */
static int matches_simple_atom(dom_doc_t *d, uint32_t id, const c_simple_atom *a) {
    dom_node_t *n = &d->nodes[id];
    if (__builtin_expect(n->type != DOM_TYPE_ELEMENT, 0)) return 0;
    if (a->tag) {
        if (a->tag_id) {
            if (n->tag_id != a->tag_id) return 0;
        } else {
            if (n->tag_len != a->tag_len) return 0;
            if (strncasecmp(NODE_BUF(d, n) + n->tag_off, a->tag, a->tag_len) != 0) return 0;
        }
    }
    if (a->n_classes > 0) {
        if (n->class_off == DOM_NIL) return 0;
        const char *cls_p = NODE_BUF(d, n) + n->class_off;
        size_t cls_len = n->class_len;
        for (int i = 0; i < a->n_classes; i++) {
            if (!class_in_attr(cls_p, cls_len, a->classes[i], a->class_lens[i])) return 0;
        }
    }
    if (a->id) {
        if (n->id_off == DOM_NIL) return 0;
        if (n->id_len != a->id_len) return 0;
        if (memcmp(NODE_BUF(d, n) + n->id_off, a->id, a->id_len) != 0) return 0;
    }
    for (int i = 0; i < a->n_attrs; i++) {
        int found = 0; size_t avl = 0; const char *avp = NULL;
        for (uint32_t k = 0; k < n->attr_count; k++) {
            dom_attr_t *ax = &d->attrs[n->attr_first + k];
            if (ax->name_len == a->attrs[i].len &&
                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, a->attrs[i].name, a->attrs[i].len) == 0) {
                avp = ATTR_BUF(d, ax) + ax->val_off; avl = ax->val_len; found = 1; break;
            }
        }
        if (!found) return 0;
        const char *vp = a->attrs[i].val; size_t vl = a->attrs[i].vlen;
        if (!attr_op_match(a->attrs[i].op, avp, avl, vp, vl, a->attrs[i].ci)) return 0;
    }
    /* Leaf pseudo-class checks. NOT/IS/HAS bits never appear on a
     * c_simple_atom (the Ruby compiler filters them out before we get
     * here) so we only handle the positional + boolean set. */
    uint32_t pf = a->pseudo_flags;
    if (__builtin_expect(pf != 0, 0)) {
        if ((pf & C_PS_FIRST_CHILD) && !is_first_element_child(d, id)) return 0;
        if ((pf & C_PS_LAST_CHILD)  && !is_last_element_child(d, id))  return 0;
        if (pf & C_PS_ONLY_CHILD) {
            if (!is_first_element_child(d, id) || !is_last_element_child(d, id)) return 0;
        }
        if ((pf & C_PS_FIRST_OF_TYPE) && !is_first_of_type(d, id)) return 0;
        if ((pf & C_PS_LAST_OF_TYPE)  && !is_last_of_type(d, id))  return 0;
        if (pf & C_PS_ONLY_OF_TYPE) {
            if (!is_first_of_type(d, id) || !is_last_of_type(d, id)) return 0;
        }
        if (pf & C_PS_EMPTY) {
            uint32_t c = d->nodes[id].first_child;
            while (c != DOM_NIL) {
                dom_node_t *m = &d->nodes[c];
                if (m->type == DOM_TYPE_ELEMENT) return 0;
                if (m->type == DOM_TYPE_TEXT && m->text_len > 0) return 0;
                c = m->next_sibling;
            }
        }
        if (pf & C_PS_ROOT) {
            uint32_t p = d->nodes[id].parent;
            if (p != DOM_NIL && d->nodes[p].type == DOM_TYPE_ELEMENT) return 0;
        }
        if ((pf & C_PS_CHECKED)   && !truthy_bool_attr(d, id, "checked", 7))   return 0;
        if ((pf & C_PS_DISABLED)  && !truthy_bool_attr(d, id, "disabled", 8))  return 0;
        if ((pf & C_PS_ENABLED)   &&  truthy_bool_attr(d, id, "disabled", 8))  return 0;
        if ((pf & C_PS_REQUIRED)  && !truthy_bool_attr(d, id, "required", 8))  return 0;
        if ((pf & C_PS_OPTIONAL)  &&  truthy_bool_attr(d, id, "required", 8))  return 0;
        if ((pf & C_PS_READ_ONLY) && !truthy_bool_attr(d, id, "readonly", 8))  return 0;
        if ((pf & C_PS_READ_WRITE)&&  truthy_bool_attr(d, id, "readonly", 8))  return 0;
        if (pf & C_PS_ANY_LINK) {
            dom_node_t *node = &d->nodes[id];
            int is_link_tag =
                (node->tag_id != 0 &&
                 (node->tag_id == DOM_TAG_ID_A || node->tag_id == DOM_TAG_ID_AREA));
            if (!is_link_tag) return 0;
            int has_href = 0;
            for (uint32_t k = 0; k < node->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[node->attr_first + k];
                if (ax->name_len == 4 &&
                    strncasecmp(ATTR_BUF(d, ax) + ax->name_off, "href", 4) == 0) {
                    has_href = 1; break;
                }
            }
            if (!has_href) return 0;
        }
        if (pf & C_PS_NTH_CHILD) {
            int idx1 = element_position_index(d, id, 0, 0);
            if (!nth_formula_matches(a->nth_a, a->nth_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_LAST_CHILD) {
            int idx1 = element_position_index(d, id, 1, 0);
            if (!nth_formula_matches(a->nth_a, a->nth_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_OF_TYPE) {
            int idx1 = element_position_index(d, id, 0, 1);
            if (!nth_formula_matches(a->nth_type_a, a->nth_type_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_LAST_OF_TYPE) {
            int idx1 = element_position_index(d, id, 1, 1);
            if (!nth_formula_matches(a->nth_type_a, a->nth_type_b, idx1)) return 0;
        }
    }
    /* One-level-deep recursive constraints lifted from `:not(...)` /
     * `:has(...)` / `:not(:has(...))` on the simple atom itself. The
     * inner atoms here are leaves (no further recursion). */
    if (a->n_inner_not > 0) {
        for (int i = 0; i < a->n_inner_not; i++) {
            if (matches_simple_atom(d, id, &a->inner_not[i])) return 0;
        }
    }
    if (a->n_inner_has > 0) {
        if (!has_descendant_matching_simple(d, id, a->inner_has, a->n_inner_has)) return 0;
    }
    if (a->n_inner_not_has > 0) {
        if (has_descendant_matching_simple(d, id, a->inner_not_has, a->n_inner_not_has)) return 0;
    }
    if (a->inner_has_chain_count > 0) {
        int offset = 0;
        int matched = 0;
        for (int c = 0; c < a->inner_has_chain_count; c++) {
            int clen = a->inner_has_chain_lens[c];
            if (has_descendant_chain_match(d, id,
                    a->inner_has_chain + offset, clen)) {
                matched = 1;
                break;
            }
            offset += clen;
        }
        if (!matched) return 0;
    }
    return 1;
}

/* For an inner :has() selector, pick the narrowest structural index
 * available and use a binary search over its (already id-sorted) entry
 * list combined with the dfs_in / dfs_out range encoding to check
 * "does this subtree contain a match" in O(log K) instead of walking
 * the whole subtree. K = number of nodes carrying the chosen class/id/
 * tag globally; on a SERP-style page that's typically a handful.
 *
 * The index entry pointer is cached on the c_simple_atom so the hash
 * lookup runs once per query, not once per candidate. On `div:has(.x)`
 * over 100 divs that cuts ~5 μs of redundant hashing. */
static int has_descendant_via_index(dom_doc_t *d, uint32_t parent_id,
                                    const c_simple_atom *a) {
    /* After an inner_html= mutation, fragment nodes get appended at
     * high ids — they aren't contiguous in the dfs_in/dfs_out range
     * of the parent anymore. Force the caller to take the
     * parent-walk fallback path, which is correct regardless of
     * ID layout. */
    if (d->tree_dirty) return -1;
    dom_index_entry_t *e = (dom_index_entry_t *)a->cached_index;
    if (e == NULL) {
        if (a->id) {
            e = dom_index_lookup(&d->id_idx, a->id, a->id_len);
            if (!e) { ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)2; return 0; }
        } else if (a->n_classes > 0) {
            for (int i = 0; i < a->n_classes; i++) {
                dom_index_entry_t *ec = dom_index_lookup(
                    &d->class_idx, a->classes[i], a->class_lens[i]);
                if (!ec) { ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)2; return 0; }
                if (!e || ec->count < e->count) e = ec;
            }
        } else if (a->tag) {
            e = dom_index_lookup(&d->tag_idx, a->tag, a->tag_len);
            if (!e) { ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)2; return 0; }
        } else {
            ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)1;
            return -1;
        }
        ((c_simple_atom *)a)->cached_index = e;
    } else if ((uintptr_t)e == 1) {
        return -1;  /* no narrowing index available */
    } else if ((uintptr_t)e == 2) {
        return 0;   /* index key not present in this document */
    }

    uint32_t parent_out = d->nodes[parent_id].dfs_out;
    /* Index ids are appended in parse (pre-order DFS) order, so the
     * list is sorted ascending. Binary-search for the first id > parent. */
    uint32_t lo = 0, hi = e->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (e->ids[mid] <= parent_id) lo = mid + 1;
        else hi = mid;
    }
    /* Trivial inner — exactly one of {id, single class, tag} and no
     * extra constraints. The chosen index entry already encodes the
     * full predicate, so the first id in range is necessarily a match. */
    int trivial = (a->pseudo_flags == 0 && a->n_attrs == 0 &&
                   ((a->id && a->n_classes == 0 && !a->tag) ||
                    (a->n_classes == 1 && !a->id && !a->tag) ||
                    (a->tag && a->n_classes == 0 && !a->id)));
    if (trivial) {
        return (lo < e->count && e->ids[lo] <= parent_out) ? 1 : 0;
    }
    /* Otherwise verify the full atom — the index may have matched on
     * just one of multiple constraints. */
    while (lo < e->count && e->ids[lo] <= parent_out) {
        if (matches_simple_atom(d, e->ids[lo], a)) return 1;
        lo++;
    }
    return 0;
}

/* :has(...) evaluation. For each inner alternative, try the index-driven
 * fast path; fall back to a subtree walk for inners with no usable
 * structural anchor (e.g. `[data-x=...]` only). */
static int has_descendant_matching_simple(dom_doc_t *d, uint32_t id,
                                          const c_simple_atom *atoms, int n) {
    for (int i = 0; i < n; i++) {
        int r = has_descendant_via_index(d, id, &atoms[i]);
        if (r > 0) return 1;
        if (r < 0) {
            /* Index unavailable for this inner — fall back to a DFS
             * scan of the subtree. */
            uint32_t parent_out = d->nodes[id].dfs_out;
            for (uint32_t k = id + 1; k <= parent_out; k++) {
                if (d->nodes[k].type == DOM_TYPE_ELEMENT &&
                    matches_simple_atom(d, k, &atoms[i])) return 1;
            }
        }
        /* r == 0: this inner has no match in the subtree; try next. */
    }
    return 0;
}

/* Verify a chain of c_simple_atoms ending at `tail_id`. Walks back
 * through ancestors with the combinator on each atom (1=descendant,
 * 2=child). The first atom has combinator 0 and is the leftmost. The
 * walk is bounded by `scope_root` — any ancestor must remain at or
 * below it (i.e. be a descendant of scope_root, inclusive). Used by
 * `:has(X Y)` to verify the chain at every candidate descendant of
 * the outer node. */
static int verify_simple_chain_backward(dom_doc_t *d, uint32_t tail_id,
                                        const c_simple_atom *atoms, int n,
                                        uint32_t scope_root) {
    if (n <= 0) return 1;
    if (!matches_simple_atom(d, tail_id, &atoms[n - 1])) return 0;
    uint32_t cur = tail_id;
    uint32_t scope_out = d->nodes[scope_root].dfs_out;
    for (int i = n - 1; i > 0; i--) {
        uint8_t combo = atoms[i].chain_combo;
        if (combo == 2) { /* child */
            uint32_t p = d->nodes[cur].parent;
            if (p == DOM_NIL || d->nodes[p].type != DOM_TYPE_ELEMENT) return 0;
            if (p <= scope_root || p > scope_out) return 0;
            if (!matches_simple_atom(d, p, &atoms[i - 1])) return 0;
            cur = p;
        } else if (combo == 3) { /* adjacent sibling: prev_sibling element */
            uint32_t s = skip_removed_backward(d, d->nodes[cur].prev_sibling);
            while (s != DOM_NIL && d->nodes[s].type != DOM_TYPE_ELEMENT) {
                s = skip_removed_backward(d, d->nodes[s].prev_sibling);
            }
            if (s == DOM_NIL) return 0;
            if (s <= scope_root || s > scope_out) return 0;
            if (!matches_simple_atom(d, s, &atoms[i - 1])) return 0;
            cur = s;
        } else if (combo == 4) { /* general sibling: any prev sibling element */
            uint32_t s = skip_removed_backward(d, d->nodes[cur].prev_sibling);
            int hit = 0;
            while (s != DOM_NIL) {
                if (d->nodes[s].type == DOM_TYPE_ELEMENT) {
                    if (s <= scope_root || s > scope_out) { s = DOM_NIL; break; }
                    if (matches_simple_atom(d, s, &atoms[i - 1])) { hit = 1; cur = s; break; }
                }
                s = skip_removed_backward(d, d->nodes[s].prev_sibling);
            }
            if (!hit) return 0;
        } else { /* descendant (treat 0 as descendant for safety) */
            uint32_t a = d->nodes[cur].parent;
            int hit = 0;
            while (a != DOM_NIL && d->nodes[a].type == DOM_TYPE_ELEMENT) {
                if (a <= scope_root || a > scope_out) break;
                if (matches_simple_atom(d, a, &atoms[i - 1])) { hit = 1; cur = a; break; }
                a = d->nodes[a].parent;
            }
            if (!hit) return 0;
        }
    }
    return 1;
}

/* `:has(X Y)` — walk the subtree of `id` looking for any descendant
 * that matches the chain `atoms[0..n-1]` (rightmost first, walking
 * ancestors for each preceding atom). Uses an index lookup on the
 * rightmost atom when available so we don't scan the entire subtree
 * for the candidates. */
static int has_descendant_chain_match(dom_doc_t *d, uint32_t id,
                                      const c_simple_atom *atoms, int n) {
    if (n <= 0) return 0;
    if (n == 1) {
        return has_descendant_matching_simple(d, id, atoms, 1);
    }
    int r = has_descendant_via_index(d, id, &atoms[n - 1]);
    if (r == 0) return 0;
    /* Walk the subtree (or the indexed candidate list) for the
     * rightmost atom, then verify the chain backward from each hit. */
    uint32_t parent_out = d->nodes[id].dfs_out;
    for (uint32_t k = id + 1; k <= parent_out; k++) {
        if (d->nodes[k].type != DOM_TYPE_ELEMENT) continue;
        if (!matches_simple_atom(d, k, &atoms[n - 1])) continue;
        if (verify_simple_chain_backward(d, k, atoms, n, id)) return 1;
    }
    return 0;
}

/* Direct-child variant of has_descendant — walks just the immediate
 * element children of `id` rather than the full subtree. Used by the
 * `:has(> X)` form. Linear in child count; for the typical SERP-style
 * page that's a handful of children per element, so even without an
 * index lookup the cost is negligible. */
static int has_direct_child_matching_simple(dom_doc_t *d, uint32_t id,
                                            const c_simple_atom *atoms, int n) {
    uint32_t c = d->nodes[id].first_child;
    while (c != DOM_NIL) {
        dom_node_t *cn = &d->nodes[c];
        if (cn->type == DOM_TYPE_ELEMENT) {
            for (int i = 0; i < n; i++) {
                if (matches_simple_atom(d, c, &atoms[i])) return 1;
            }
        }
        c = cn->next_sibling;
    }
    return 0;
}

static int element_matches_atom(dom_doc_t *d, uint32_t id, const c_atom *a) {
    dom_node_t *n = &d->nodes[id];
    if (__builtin_expect(n->type != DOM_TYPE_ELEMENT, 0)) return 0;
    if (a->tag) {
        if (a->tag_id) {
            if (n->tag_id != a->tag_id) return 0;
        } else {
            if (n->tag_len != a->tag_len) return 0;
            if (strncasecmp(NODE_BUF(d, n) + n->tag_off, a->tag, a->tag_len) != 0) return 0;
        }
    }
    if (a->n_classes > 0 || a->id || a->n_attrs > 0) {
        /* Class/id checks read from the cached spans on the node —
         * populated at parse time so this is O(1) instead of scanning
         * the attribute array on every match. */
        if (a->n_classes > 0) {
            if (n->class_off == DOM_NIL) return 0;
            const char *cls_p = NODE_BUF(d, n) + n->class_off;
            size_t cls_len = n->class_len;
            for (int i = 0; i < a->n_classes; i++) {
                if (!class_in_attr(cls_p, cls_len, a->classes[i], a->class_lens[i])) return 0;
            }
        }
        if (a->id) {
            if (n->id_off == DOM_NIL) return 0;
            if (n->id_len != a->id_len) return 0;
            if (memcmp(NODE_BUF(d, n) + n->id_off, a->id, a->id_len) != 0) return 0;
        }
        for (int i = 0; i < a->n_attrs; i++) {
            int found = 0; size_t avl = 0; const char *avp = NULL;
            for (uint32_t k = 0; k < n->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[n->attr_first + k];
                if (ax->name_len == a->attrs[i].len &&
                    strncasecmp(ATTR_BUF(d, ax) + ax->name_off, a->attrs[i].name, a->attrs[i].len) == 0) {
                    avp = ATTR_BUF(d, ax) + ax->val_off; avl = ax->val_len; found = 1; break;
                }
            }
            if (!found) return 0;
            const char *vp = a->attrs[i].val; size_t vl = a->attrs[i].vlen;
            if (!attr_op_match(a->attrs[i].op, avp, avl, vp, vl, a->attrs[i].ci)) return 0;
        }
    }

    /* Pseudo-class evaluation. Cheap bit-test in the common case where
     * the atom has no pseudos at all. */
    uint32_t pf = a->pseudo_flags;
    if (__builtin_expect(pf != 0, 0)) {
        if ((pf & C_PS_FIRST_CHILD) && !is_first_element_child(d, id)) return 0;
        if ((pf & C_PS_LAST_CHILD)  && !is_last_element_child(d, id))  return 0;
        if (pf & C_PS_ONLY_CHILD) {
            if (!is_first_element_child(d, id) || !is_last_element_child(d, id)) return 0;
        }
        if ((pf & C_PS_FIRST_OF_TYPE) && !is_first_of_type(d, id)) return 0;
        if ((pf & C_PS_LAST_OF_TYPE)  && !is_last_of_type(d, id))  return 0;
        if (pf & C_PS_ONLY_OF_TYPE) {
            if (!is_first_of_type(d, id) || !is_last_of_type(d, id)) return 0;
        }
        if (pf & C_PS_EMPTY) {
            uint32_t c = n->first_child;
            while (c != DOM_NIL) {
                dom_node_t *m = &d->nodes[c];
                if (m->type == DOM_TYPE_ELEMENT) return 0;
                if (m->type == DOM_TYPE_TEXT && m->text_len > 0) return 0;
                c = m->next_sibling;
            }
        }
        if (pf & C_PS_ROOT) {
            uint32_t p = n->parent;
            /* "root" means the document element — its parent is the
             * document node (type DOM_TYPE_DOC), not another element. */
            if (p != DOM_NIL && d->nodes[p].type == DOM_TYPE_ELEMENT) return 0;
        }
        if (pf & C_PS_CHECKED) {
            if (!truthy_bool_attr(d, id, "checked", 7)) return 0;
        }
        if (pf & C_PS_DISABLED) {
            if (!truthy_bool_attr(d, id, "disabled", 8)) return 0;
        }
        if (pf & C_PS_ENABLED) {
            if (truthy_bool_attr(d, id, "disabled", 8)) return 0;
        }
        if (pf & C_PS_REQUIRED) {
            if (!truthy_bool_attr(d, id, "required", 8)) return 0;
        }
        if (pf & C_PS_OPTIONAL) {
            if (truthy_bool_attr(d, id, "required", 8)) return 0;
        }
        if (pf & C_PS_READ_ONLY) {
            if (!truthy_bool_attr(d, id, "readonly", 8)) return 0;
        }
        if (pf & C_PS_READ_WRITE) {
            if (truthy_bool_attr(d, id, "readonly", 8)) return 0;
        }
        if (pf & C_PS_ANY_LINK) {
            /* :any-link / :link — <a> or <area> with an href. */
            int is_link_tag =
                (n->tag_len == 1 && (NODE_BUF(d, n)[n->tag_off] == 'a' || NODE_BUF(d, n)[n->tag_off] == 'A')) ||
                (n->tag_len == 4 && strncasecmp(NODE_BUF(d, n) + n->tag_off, "area", 4) == 0);
            if (!is_link_tag) return 0;
            int has_href = 0;
            for (uint32_t k = 0; k < n->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[n->attr_first + k];
                if (ax->name_len == 4 &&
                    strncasecmp(ATTR_BUF(d, ax) + ax->name_off, "href", 4) == 0) {
                    has_href = 1; break;
                }
            }
            if (!has_href) return 0;
        }
        if (pf & C_PS_NTH_CHILD) {
            int idx1 = element_position_index(d, id, 0, 0);
            if (!nth_formula_matches(a->nth_a, a->nth_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_LAST_CHILD) {
            int idx1 = element_position_index(d, id, 1, 0);
            if (!nth_formula_matches(a->nth_a, a->nth_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_OF_TYPE) {
            int idx1 = element_position_index(d, id, 0, 1);
            if (!nth_formula_matches(a->nth_type_a, a->nth_type_b, idx1)) return 0;
        }
        if (pf & C_PS_NTH_LAST_OF_TYPE) {
            int idx1 = element_position_index(d, id, 1, 1);
            if (!nth_formula_matches(a->nth_type_a, a->nth_type_b, idx1)) return 0;
        }
        if (pf & C_PS_NOT) {
            for (int i = 0; i < a->n_not_inner; i++) {
                if (matches_simple_atom(d, id, &a->not_inner[i])) return 0;
            }
        }
        if (pf & C_PS_IS) {
            int hit = 0;
            for (int i = 0; i < a->n_is_inner; i++) {
                if (matches_simple_atom(d, id, &a->is_inner[i])) { hit = 1; break; }
            }
            if (!hit) return 0;
        }
        if (pf & C_PS_HAS) {
            if (!has_descendant_matching_simple(d, id, a->has_inner, a->n_has_inner)) return 0;
        }
        if (pf & C_PS_NOT_HAS) {
            if (has_descendant_matching_simple(d, id, a->not_has_inner, a->n_not_has_inner)) return 0;
        }
        if (pf & C_PS_HAS_CHILD) {
            if (!has_direct_child_matching_simple(d, id, a->has_child_inner, a->n_has_child_inner)) return 0;
        }
        if (pf & C_PS_NOT_HAS_CHILD) {
            if (has_direct_child_matching_simple(d, id, a->not_has_child_inner, a->n_not_has_child_inner)) return 0;
        }
        if (pf & C_PS_HAS_CHAIN) {
            int offset = 0;
            int matched = 0;
            for (int c = 0; c < a->has_chain_count; c++) {
                int clen = a->has_chain_lens[c];
                if (has_descendant_chain_match(d, id,
                        a->has_chain_inner + offset, clen)) {
                    matched = 1;
                    break;
                }
                offset += clen;
            }
            if (!matched) return 0;
        }
        if (pf & C_PS_NOT_HAS_CHAIN) {
            int offset = 0;
            int matched = 0;
            for (int c = 0; c < a->not_has_chain_count; c++) {
                int clen = a->not_has_chain_lens[c];
                if (has_descendant_chain_match(d, id,
                        a->not_has_chain_inner + offset, clen)) {
                    matched = 1;
                    break;
                }
                offset += clen;
            }
            if (matched) return 0;
        }
        if (pf & C_PS_HAS_NEXT_SIB) {
            uint32_t s = skip_removed_forward(d, d->nodes[id].next_sibling);
            while (s != DOM_NIL && d->nodes[s].type != DOM_TYPE_ELEMENT) {
                s = skip_removed_forward(d, d->nodes[s].next_sibling);
            }
            int hit = 0;
            if (s != DOM_NIL) {
                for (int i = 0; i < a->n_has_inner; i++) {
                    if (matches_simple_atom(d, s, &a->has_inner[i])) { hit = 1; break; }
                }
            }
            if (!hit) return 0;
        }
        if (pf & C_PS_HAS_LATER_SIB) {
            uint32_t s = skip_removed_forward(d, d->nodes[id].next_sibling);
            int hit = 0;
            while (s != DOM_NIL) {
                if (d->nodes[s].type == DOM_TYPE_ELEMENT) {
                    for (int i = 0; i < a->n_has_inner; i++) {
                        if (matches_simple_atom(d, s, &a->has_inner[i])) { hit = 1; break; }
                    }
                    if (hit) break;
                }
                s = skip_removed_forward(d, d->nodes[s].next_sibling);
            }
            if (!hit) return 0;
        }
        if (pf & C_PS_HAS_TEXT_CHILD) {
            uint32_t c = d->nodes[id].first_child;
            int hit = 0;
            while (c != DOM_NIL) {
                dom_node_t *cn = &d->nodes[c];
                if (cn->type == DOM_TYPE_TEXT && cn->text_len > 0) {
                    /* Non-whitespace check: walk the bytes once. */
                    const char *p = NODE_BUF(d, cn) + cn->text_off;
                    size_t L = cn->text_len;
                    for (size_t k = 0; k < L; k++) {
                        unsigned char b = (unsigned char)p[k];
                        if (b != ' ' && b != '\t' && b != '\n' && b != '\r' && b != '\f') { hit = 1; break; }
                    }
                    if (hit) break;
                }
                c = cn->next_sibling;
            }
            if (!hit) return 0;
        }
        /* C_PS_SCOPE has no effect on matching — it identifies the
         * current scope, which is already enforced by the candidate set. */
    }
    return 1;
}

static int match_chain_backward(dom_doc_t *d, uint32_t node_id, c_atom *atoms, int n_atoms, int idx, uint32_t scope_id) {
    if (idx < 0) return 1;
    int combinator = atoms[idx + 1].combinator;  /* combinator linking idx -> idx+1 */
    if (combinator == 2 /* child */) {
        uint32_t p = d->nodes[node_id].parent;
        if (p == DOM_NIL || d->nodes[p].type != DOM_TYPE_ELEMENT) return 0;
        if (scope_id != DOM_NIL) {
            uint32_t cur = p;
            int in_scope = 0;
            while (cur != DOM_NIL) {
                if (cur == scope_id) { in_scope = 1; break; }
                cur = d->nodes[cur].parent;
            }
            if (!in_scope) return 0;
        }
        if (!element_matches_atom(d, p, &atoms[idx])) return 0;
        return match_chain_backward(d, p, atoms, n_atoms, idx - 1, scope_id);
    }
    if (combinator == 3 /* adjacent sibling: A + B */) {
        uint32_t s = skip_removed_backward(d, d->nodes[node_id].prev_sibling);
        while (s != DOM_NIL && d->nodes[s].type != DOM_TYPE_ELEMENT) {
            s = skip_removed_backward(d, d->nodes[s].prev_sibling);
        }
        if (s == DOM_NIL) return 0;
        if (scope_id != DOM_NIL) {
            uint32_t cur = s;
            int in_scope = 0;
            while (cur != DOM_NIL) {
                if (cur == scope_id) { in_scope = 1; break; }
                cur = d->nodes[cur].parent;
            }
            if (!in_scope) return 0;
        }
        if (!element_matches_atom(d, s, &atoms[idx])) return 0;
        return match_chain_backward(d, s, atoms, n_atoms, idx - 1, scope_id);
    }
    if (combinator == 4 /* general sibling: A ~ B */) {
        uint32_t s = skip_removed_backward(d, d->nodes[node_id].prev_sibling);
        while (s != DOM_NIL) {
            if (d->nodes[s].type == DOM_TYPE_ELEMENT) {
                int in_scope = (scope_id == DOM_NIL);
                if (!in_scope) {
                    uint32_t cur = s;
                    while (cur != DOM_NIL) {
                        if (cur == scope_id) { in_scope = 1; break; }
                        cur = d->nodes[cur].parent;
                    }
                }
                if (in_scope && element_matches_atom(d, s, &atoms[idx])
                    && match_chain_backward(d, s, atoms, n_atoms, idx - 1, scope_id)) {
                    return 1;
                }
            }
            s = skip_removed_backward(d, d->nodes[s].prev_sibling);
        }
        return 0;
    }
    /* descendant */
    uint32_t cur = d->nodes[node_id].parent;
    while (cur != DOM_NIL && d->nodes[cur].type == DOM_TYPE_ELEMENT) {
        int in_scope = (scope_id == DOM_NIL);
        if (!in_scope) {
            uint32_t c = cur;
            while (c != DOM_NIL) {
                if (c == scope_id) { in_scope = 1; break; }
                c = d->nodes[c].parent;
            }
        }
        if (in_scope && element_matches_atom(d, cur, &atoms[idx])
            && match_chain_backward(d, cur, atoms, n_atoms, idx - 1, scope_id)) {
            return 1;
        }
        cur = d->nodes[cur].parent;
    }
    return 0;
}

/* Run a compiled selector chain over the document — returns a Ruby Array
 * of node ids. When `limit_n > 0` the iteration stops after that many
 * matches, so `at_css` can run a chain with limit=1 without paying the
 * cost of finding every match. limit_n == -1 means "no limit". */
static VALUE dom_run_chain_impl(VALUE self, VALUE plan_v, VALUE scope_v, long limit_n) {
    dom_doc_t *d = get_dom(self);
    if (!RB_TYPE_P(plan_v, T_ARRAY)) rb_raise(rb_eArgError, "plan must be Array");
    long n = RARRAY_LEN(plan_v);
    if (n == 0) return rb_ary_new();

    uint32_t scope_id = NIL_P(scope_v) ? DOM_NIL : NUM2UINT(scope_v);
    (void)limit_n;

    c_atom *atoms = (c_atom *)alloca(sizeof(c_atom) * n);
    long n_inner = count_inner_atoms(plan_v);
    c_simple_atom *inner_pool = NULL;
    if (n_inner > 0) {
        inner_pool = (c_simple_atom *)alloca(sizeof(c_simple_atom) * (size_t)n_inner);
    }
    long pool_used = 0;
    for (long i = 0; i < n; i++) {
        VALUE entry = rb_ary_entry(plan_v, i);
        if (!RB_TYPE_P(entry, T_ARRAY) || RARRAY_LEN(entry) < 2) rb_raise(rb_eArgError, "bad plan entry");
        VALUE sel = rb_ary_entry(entry, 0);
        VALUE combo = rb_ary_entry(entry, 1);
        if (!build_atom(sel, &atoms[i])) rb_raise(rb_eArgError, "bad selector atom");
        if (!build_atom_pseudos(sel, &atoms[i], inner_pool, &pool_used)) {
            rb_raise(rb_eArgError, "bad pseudo data");
        }
        if (NIL_P(combo))                      atoms[i].combinator = 0;
        else if (RB_TYPE_P(combo, T_STRING)) {
            long cl = RSTRING_LEN(combo);
            const char *cp = RSTRING_PTR(combo);
            if      (cl == 10 && memcmp(cp, "descendant", 10) == 0) atoms[i].combinator = 1;
            else if (cl == 5  && memcmp(cp, "child", 5) == 0)       atoms[i].combinator = 2;
            else if (cl == 8  && memcmp(cp, "adjacent", 8) == 0)    atoms[i].combinator = 3;
            else if (cl == 7  && memcmp(cp, "sibling", 7) == 0)     atoms[i].combinator = 4;
            else rb_raise(rb_eArgError, "bad combinator");
        } else {
            rb_raise(rb_eArgError, "bad combinator type");
        }
    }

    /* Candidates for the last (rightmost) atom: use the narrowest index. */
    c_atom *last = &atoms[n - 1];
    uint32_t *cands = NULL;
    size_t n_cands = 0;
    int cands_owned = 0;  /* must free if 1 */

    if (last->id) {
        dom_index_entry_t *e = dom_index_lookup(&d->id_idx, last->id, last->id_len);
        if (e) { cands = e->ids; n_cands = e->count; }
    } else if (last->n_classes > 0) {
        /* pick smallest class set */
        dom_index_entry_t *best = NULL;
        for (int i = 0; i < last->n_classes; i++) {
            dom_index_entry_t *e = dom_index_lookup(&d->class_idx, last->classes[i], last->class_lens[i]);
            if (!e) { best = NULL; break; }
            if (!best || e->count < best->count) best = e;
        }
        if (best) { cands = best->ids; n_cands = best->count; }
    } else if (last->tag) {
        dom_index_entry_t *e = dom_index_lookup(&d->tag_idx, last->tag, last->tag_len);
        if (e) { cands = e->ids; n_cands = e->count; }
    } else if (last->n_attrs > 0) {
        /* No tag/class/id constraint, but we have attribute selectors.
         * The narrowest candidate set is "elements that actually carry
         * the first attribute name". Built lazily and cached. */
        dom_index_entry_t *e = dom_attr_candidates(
            d, last->attrs[0].name, last->attrs[0].len);
        if (e) { cands = e->ids; n_cands = e->count; }
    } else {
        /* True universal selector (`*`): every element. */
        cands = (uint32_t *)malloc(sizeof(uint32_t) * d->n_nodes);
        cands_owned = 1;
        for (uint32_t i = 0; i < d->n_nodes; i++) {
            if (d->nodes[i].type == DOM_TYPE_ELEMENT) cands[n_cands++] = i;
        }
    }

    /* Collect matched ids into a stack buffer, then build the Ruby
     * Array once at the end. Saves 50+ rb_ary_push function calls per
     * query on the typical listing workload. Spills to heap for
     * unusually large candidate sets. */
    enum { STACK_RESULT_CAP = 256 };
    VALUE  stack_buf[STACK_RESULT_CAP];
    VALUE *values = stack_buf;
    size_t values_cap = STACK_RESULT_CAP;
    size_t n_values = 0;
    int    values_on_heap = 0;

#define EMIT_ID(_id)                                                  \
    do {                                                              \
        if (n_values == values_cap) {                                 \
            values_cap *= 2;                                          \
            if (!values_on_heap) {                                    \
                VALUE *_nb = (VALUE *)malloc(sizeof(VALUE) * values_cap); \
                memcpy(_nb, values, sizeof(VALUE) * n_values);        \
                values = _nb; values_on_heap = 1;                     \
            } else {                                                  \
                values = (VALUE *)realloc(values, sizeof(VALUE) * values_cap); \
            }                                                         \
        }                                                             \
        values[n_values++] = UINT2NUM(_id);                           \
        if (limit_n > 0 && (long)n_values >= limit_n) goto run_chain_done; \
    } while (0)

    /* When the candidate set was chosen from a structural index whose
     * key fully encodes the rightmost atom's predicate (no extra
     * classes / attrs / pseudos), every candidate already satisfies
     * `last`. We can skip the per-candidate element_matches_atom call.
     *
     *   `.card`           -> class_index[card] is already exact
     *   `#main`           -> id_index[main]    is the unique match
     *   `article`         -> tag_index[article] is already exact
     *
     * Cuts ~50 ns/candidate × 50-100 candidates per query, which is
     * worth several μs on the listing workload. */
    int last_pre_matched =
        (last->n_attrs == 0 && last->pseudo_flags == 0 &&
         ((last->id     && !last->tag && last->n_classes == 0) ||
          (last->n_classes == 1 && !last->tag && !last->id) ||
          (last->tag    && last->n_classes == 0 && !last->id)));
    /* When n=1 and no scope, the bypass extends all the way to "emit
     * candidates directly without iterating" — we know the answer is
     * exactly the candidate set. */
    int prefilter_bypass = (last_pre_matched && n == 1 && scope_id == DOM_NIL);

    /* tag.class: candidates came from class_index (since it's narrower
     * than tag_index for a single class). The class is verified by the
     * choice of index; only the tag needs checking. ~5 ns per candidate
     * instead of ~15 ns for the full element_matches_atom path. */
    int tag_class_bypass =
        (n == 1 && scope_id == DOM_NIL &&
         last->tag && last->n_classes == 1 && !last->id &&
         last->n_attrs == 0 && last->pseudo_flags == 0);

    /* `.A:not(.B)`: set difference between class_index[A] and class_index[B].
     * Both indexes are sorted by id, so a single merge walk computes the
     * result in O(|A| + |B|) without any per-candidate predicate eval.
     * This is the hot pattern for "all matching cards that aren't
     * disabled / removed / hidden / etc." — scraping parsers use it
     * heavily and the candidate-set verify path makes it ~3x; this
     * cuts the loop down to a couple of dependent loads per candidate. */
    int set_diff_bypass =
        (n == 1 && scope_id == DOM_NIL &&
         last->n_classes == 1 && !last->tag && !last->id &&
         last->n_attrs == 0 &&
         last->pseudo_flags == C_PS_NOT &&
         last->n_not_inner == 1 &&
         last->not_inner[0].n_classes == 1 &&
         !last->not_inner[0].tag &&
         !last->not_inner[0].id &&
         last->not_inner[0].n_attrs == 0 &&
         last->not_inner[0].pseudo_flags == 0);

    /* `.card:first-child`, `.video-item:nth-of-type(2n)` and friends:
     * candidate set from class_index already verifies the class, so the
     * inner loop only needs to evaluate the positional / boolean
     * pseudos. No tag/id/attrs in play. We still go through
     * element_matches_atom to handle the pseudo bitmap, but skipping
     * the tag/class/attr loops up front shaves a noticeable chunk per
     * candidate on the SERP-style mixed workload. */
    int class_with_leaf_pseudos_bypass =
        (n == 1 && scope_id == DOM_NIL &&
         last->n_classes == 1 && !last->tag && !last->id &&
         last->n_attrs == 0 &&
         last->pseudo_flags != 0 &&
         (last->pseudo_flags & (C_PS_NOT | C_PS_IS | C_PS_HAS | C_PS_NOT_HAS |
                                C_PS_HAS_CHILD | C_PS_NOT_HAS_CHILD |
                                C_PS_HAS_CHAIN | C_PS_HAS_TEXT_CHILD |
                                C_PS_NOT_HAS_CHAIN |
                                C_PS_HAS_NEXT_SIB | C_PS_HAS_LATER_SIB)) == 0);

    /* Ancestor-bloom mask: bits we need to find on the candidate's
     * ancestor chain. Walk back from the rightmost atom, accumulating
     * each prior atom's self_bloom — but only as long as the combinator
     * is descendant (1) or child (2). A sibling combinator (3/4) breaks
     * the chain because the corresponding atom is not an ancestor. */
    uint64_t required_ancestor_bloom = 0;
    if (n >= 2) {
        for (int ai = n - 1; ai > 0; ai--) {
            int co = atoms[ai].combinator;
            if (co == 1 || co == 2) {
                required_ancestor_bloom |= atoms[ai - 1].self_bloom;
            } else {
                break;
            }
        }
    }

    if (prefilter_bypass) {
        /* Reserve space upfront. The candidate count is the exact answer. */
        if (n_cands > values_cap) {
            values_cap = n_cands;
            if (!values_on_heap) {
                values = (VALUE *)malloc(sizeof(VALUE) * values_cap);
                values_on_heap = 1;
            } else {
                values = (VALUE *)realloc(values, sizeof(VALUE) * values_cap);
            }
        }
        if (d->has_removed) {
            for (size_t i = 0; i < n_cands; i++) {
                if (d->nodes[cands[i]].type == DOM_TYPE_REMOVED) continue;
                values[n_values++] = UINT2NUM(cands[i]);
            }
        } else {
            for (size_t i = 0; i < n_cands; i++) {
                values[n_values++] = UINT2NUM(cands[i]);
            }
        }
    } else if (set_diff_bypass) {
        dom_index_entry_t *e_not =
            dom_index_lookup(&d->class_idx,
                             last->not_inner[0].classes[0],
                             last->not_inner[0].class_lens[0]);
        if (e_not == NULL) {
            /* No nodes carry the excluded class — every candidate passes. */
            if (n_cands > values_cap) {
                values_cap = n_cands;
                if (!values_on_heap) {
                    values = (VALUE *)malloc(sizeof(VALUE) * values_cap);
                    values_on_heap = 1;
                } else {
                    values = (VALUE *)realloc(values, sizeof(VALUE) * values_cap);
                }
            }
            for (size_t i = 0; i < n_cands; i++) {
                if (d->has_removed && d->nodes[cands[i]].type == DOM_TYPE_REMOVED) continue;
                values[n_values++] = UINT2NUM(cands[i]);
            }
        } else {
            /* Merge walk: both lists sorted by id (insertion order at parse). */
            size_t dis_pos = 0;
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                while (dis_pos < e_not->count && e_not->ids[dis_pos] < id) dis_pos++;
                if (dis_pos < e_not->count && e_not->ids[dis_pos] == id) continue;
                if (d->has_removed && d->nodes[id].type == DOM_TYPE_REMOVED) continue;
                EMIT_ID(id);
            }
        }
    } else if (tag_class_bypass) {
        for (size_t i = 0; i < n_cands; i++) {
            dom_node_t *cn = &d->nodes[cands[i]];
            if (d->has_removed && cn->type == DOM_TYPE_REMOVED) continue;
            if (cn->tag_len != last->tag_len) continue;
            if (strncasecmp(NODE_BUF(d, cn) + cn->tag_off, last->tag, last->tag_len) != 0) continue;
            EMIT_ID(cands[i]);
        }
    } else if (class_with_leaf_pseudos_bypass) {
        /* Class already verified by index choice; only the positional /
         * boolean pseudos need checking per candidate. Inline the bitmap
         * test for the common cases so the loop is one or two
         * conditional reads per candidate. */
        uint32_t pf = last->pseudo_flags;
        for (size_t i = 0; i < n_cands; i++) {
            uint32_t id = cands[i];
            if (d->has_removed && d->nodes[id].type == DOM_TYPE_REMOVED) continue;
            if ((pf & C_PS_FIRST_CHILD) && !is_first_element_child(d, id)) continue;
            if ((pf & C_PS_LAST_CHILD)  && !is_last_element_child(d, id))  continue;
            if ((pf & C_PS_ONLY_CHILD)  &&
                (!is_first_element_child(d, id) || !is_last_element_child(d, id))) continue;
            if ((pf & C_PS_FIRST_OF_TYPE) && !is_first_of_type(d, id)) continue;
            if ((pf & C_PS_LAST_OF_TYPE)  && !is_last_of_type(d, id))  continue;
            if ((pf & C_PS_ONLY_OF_TYPE)  &&
                (!is_first_of_type(d, id) || !is_last_of_type(d, id))) continue;
            if ((pf & C_PS_NTH_CHILD) &&
                !nth_formula_matches(last->nth_a, last->nth_b,
                                      element_position_index(d, id, 0, 0))) continue;
            if ((pf & C_PS_NTH_LAST_CHILD) &&
                !nth_formula_matches(last->nth_a, last->nth_b,
                                      element_position_index(d, id, 1, 0))) continue;
            if ((pf & C_PS_NTH_OF_TYPE) &&
                !nth_formula_matches(last->nth_type_a, last->nth_type_b,
                                      element_position_index(d, id, 0, 1))) continue;
            if ((pf & C_PS_NTH_LAST_OF_TYPE) &&
                !nth_formula_matches(last->nth_type_a, last->nth_type_b,
                                      element_position_index(d, id, 1, 1))) continue;
            /* Less-common: empty/root/boolean attr — defer to the full
             * matcher; rare enough that the extra call doesn't matter. */
            if (pf & (C_PS_EMPTY | C_PS_ROOT | C_PS_CHECKED | C_PS_DISABLED |
                      C_PS_ENABLED | C_PS_REQUIRED | C_PS_OPTIONAL |
                      C_PS_READ_ONLY | C_PS_READ_WRITE | C_PS_ANY_LINK)) {
                if (!element_matches_atom(d, id, last)) continue;
            }
            EMIT_ID(id);
        }
    } else if (scope_id == DOM_NIL) {
        if (n == 1) {
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (!element_matches_atom(d, id, last)) continue;
                EMIT_ID(id);
            }
        } else if (n == 2 && atoms[1].combinator == 2 /* child */) {
            /* Specialised n=2 child path: A > B. Walk one parent
             * pointer per candidate, match it inline. */
            c_atom *left = &atoms[0];
            /* `#X > Y`: anchor is a unique id. Skip the per-candidate
             * element_matches_atom for the parent — just compare parent
             * id to the anchor id directly. */
            int left_is_pure_id =
                (left->id && !left->tag && left->n_classes == 0 &&
                 left->n_attrs == 0 && left->pseudo_flags == 0);
            uint32_t anchor_id = DOM_NIL;
            if (left_is_pure_id) {
                dom_index_entry_t *id_e = dom_index_lookup(
                    &d->id_idx, left->id, left->id_len);
                if (id_e && id_e->count > 0) anchor_id = id_e->ids[0];
                else { /* anchor doesn't exist — no matches */
                    goto skip_n2_child;
                }
            }
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (required_ancestor_bloom != 0 &&
                    (d->nodes[id].ancestor_bloom & required_ancestor_bloom) != required_ancestor_bloom) continue;
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                uint32_t p = d->nodes[id].parent;
                if (p == DOM_NIL) continue;
                if (left_is_pure_id) {
                    if (p != anchor_id) continue;
                } else if (!element_matches_atom(d, p, left)) continue;
                EMIT_ID(id);
            }
            skip_n2_child:;
        } else if (n == 2 && atoms[1].combinator == 1 /* descendant */) {
            /* Specialised n=2 descendant path: A B. */
            c_atom *left = &atoms[0];
            /* `#X Y`: anchor is a unique id. Replace the parent-walk
             * with an O(1) dfs-range check (id < cand <= id.dfs_out). */
            int left_is_pure_id =
                (left->id && !left->tag && left->n_classes == 0 &&
                 left->n_attrs == 0 && left->pseudo_flags == 0);
            /* `.A .B`: left is one class, no other constraints. Inline
             * the class_in_attr check on each ancestor so we don't pay
             * the element_matches_atom call overhead per parent step.
             * Works in tandem with last_pre_matched so the B candidate's
             * own class isn't re-verified. */
            int left_is_pure_class =
                (last_pre_matched &&
                 left->n_classes == 1 && !left->tag && !left->id &&
                 left->n_attrs == 0 && left->pseudo_flags == 0);
            if (left_is_pure_id) {
                dom_index_entry_t *id_e = dom_index_lookup(
                    &d->id_idx, left->id, left->id_len);
                if (!id_e || id_e->count == 0) goto skip_n2_desc;
                uint32_t anchor_id = id_e->ids[0];
                uint32_t anchor_out = d->nodes[anchor_id].dfs_out;
                for (size_t i = 0; i < n_cands; i++) {
                    uint32_t id = cands[i];
                    if (id <= anchor_id || id > anchor_out) continue;
                    if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                    EMIT_ID(id);
                }
                goto skip_n2_desc;
            }
            if (left_is_pure_class) {
                const char *lcls = left->classes[0];
                size_t lclen = left->class_lens[0];
                for (size_t i = 0; i < n_cands; i++) {
                    uint32_t id = cands[i];
                    if (required_ancestor_bloom != 0 &&
                        (d->nodes[id].ancestor_bloom & required_ancestor_bloom) != required_ancestor_bloom) continue;
                    uint32_t cur = d->nodes[id].parent;
                    while (cur != DOM_NIL) {
                        dom_node_t *cn = &d->nodes[cur];
                        if (cn->type != DOM_TYPE_ELEMENT) break;
                        if (cn->class_off != DOM_NIL &&
                            class_in_attr(NODE_BUF(d, cn) + cn->class_off, cn->class_len, lcls, lclen)) {
                            EMIT_ID(id);
                            break;
                        }
                        cur = cn->parent;
                    }
                }
                goto skip_n2_desc;
            }
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (required_ancestor_bloom != 0 &&
                    (d->nodes[id].ancestor_bloom & required_ancestor_bloom) != required_ancestor_bloom) continue;
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                uint32_t cur = d->nodes[id].parent;
                int matched = 0;
                while (cur != DOM_NIL && d->nodes[cur].type == DOM_TYPE_ELEMENT) {
                    if (element_matches_atom(d, cur, left)) { matched = 1; break; }
                    cur = d->nodes[cur].parent;
                }
                if (matched) EMIT_ID(id);
            }
            skip_n2_desc:;
        } else {
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (required_ancestor_bloom != 0 &&
                    (d->nodes[id].ancestor_bloom & required_ancestor_bloom) != required_ancestor_bloom) continue;
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                if (!match_chain_backward(d, id, atoms, (int)n, (int)n - 2, DOM_NIL)) continue;
                EMIT_ID(id);
            }
        }
    } else if (!prefilter_bypass) {
        for (size_t i = 0; i < n_cands; i++) {
            uint32_t id = cands[i];
            if (required_ancestor_bloom != 0 &&
                (d->nodes[id].ancestor_bloom & required_ancestor_bloom) != required_ancestor_bloom) continue;
            if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
            /* In-scope check. */
            int in_scope = 0;
            uint32_t c = id;
            while (c != DOM_NIL) {
                if (c == scope_id) { in_scope = 1; break; }
                c = d->nodes[c].parent;
            }
            if (!in_scope) continue;
            if (n > 1 && !match_chain_backward(d, id, atoms, (int)n, (int)n - 2, scope_id)) continue;
            EMIT_ID(id);
        }
    }

run_chain_done:
#undef EMIT_ID

    /* One allocation + memcpy instead of N pushes. */
    VALUE result = (n_values == 0) ? rb_ary_new() : rb_ary_new_from_values((long)n_values, values);
    if (values_on_heap) free(values);
    if (cands_owned) free(cands);
    return result;
}

static VALUE dom_run_chain(VALUE self, VALUE plan_v, VALUE scope_v) {
    return dom_run_chain_impl(self, plan_v, scope_v, -1);
}

/* Find the first matching id without paying for the full result list.
 * Returns Integer id or Qnil. Used by Element#at_css's super-fast path
 * — the dispatch shaves the Array allocation, the limit prunes the
 * search early. */
static VALUE dom_first_match(VALUE self, VALUE plan_v, VALUE scope_v) {
    VALUE ids = dom_run_chain_impl(self, plan_v, scope_v, 1);
    if (RARRAY_LEN(ids) == 0) return Qnil;
    return rb_ary_entry(ids, 0);
}

/* Full at_css dispatch in C. Args: (scope_id, selector_str, wrapper).
 * Returns Integer id (match), Qnil (no match), or Qtrue (sentinel:
 * "use the Ruby slow path"). Skips the Ruby method-call chain on the
 * very-hot path where the selector is a single-group String, the
 * compile cache has a plan, and we just need the first match. */
static VALUE dom_fast_at_css(VALUE self, VALUE scope_v, VALUE selector_v, VALUE wrapper_v) {
    if (!RB_TYPE_P(selector_v, T_STRING)) return Qtrue;
    long sl = RSTRING_LEN(selector_v);
    const char *sp = RSTRING_PTR(selector_v);
    for (long i = 0; i < sl; i++) {
        if (sp[i] == ',') return Qtrue;
        if (sp[i] == ':' && i + 1 < sl && sp[i + 1] == ':') return Qtrue;
    }
    if (NIL_P(wrapper_v)) return Qtrue;

    /* Inline @compile_cache hash lookup. Avoids the compiled_plan
     * method dispatch on the cache-hit fast path. */
    static ID iv_compile_cache = 0;
    if (!iv_compile_cache) iv_compile_cache = rb_intern("@compile_cache");
    VALUE cache = rb_ivar_get(wrapper_v, iv_compile_cache);
    VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, selector_v);
    if (plan == Qfalse) return Qtrue;
    if (NIL_P(plan)) {
        static ID id_compiled_plan = 0;
        if (!id_compiled_plan) id_compiled_plan = rb_intern("compiled_plan");
        plan = rb_funcall(wrapper_v, id_compiled_plan, 1, selector_v);
        if (NIL_P(plan)) return Qtrue;
    }
    VALUE ids = dom_run_chain_impl(self, plan, scope_v, 1);
    if (RARRAY_LEN(ids) == 0) return Qnil;
    return rb_ary_entry(ids, 0);
}

/* Helper: look up (selector_str, scope_id) in the shared result cache
 * on the dom_doc_t. Returns the cached ids Array, or Qnil on miss /
 * if caching is disabled for this Document. */
static inline VALUE result_cache_get(dom_doc_t *d, VALUE selector_v, VALUE scope_v) {
    if (d->cache_disabled || NIL_P(d->shared_result_cache)) return Qnil;
    VALUE key = rb_ary_new_from_args(2, selector_v, scope_v);
    return rb_hash_aref(d->shared_result_cache, key);
}
static inline void result_cache_put(dom_doc_t *d, VALUE selector_v, VALUE scope_v, VALUE ids) {
    if (d->cache_disabled || NIL_P(d->shared_result_cache)) return;
    /* Freeze the key + the ids array so callers can't mutate cached
     * entries by accident. The key + value are interned in the shared
     * Hash; subsequent identical lookups return the same frozen Array. */
    VALUE key = rb_ary_new_from_args(2, selector_v, scope_v);
    rb_obj_freeze(key);
    rb_obj_freeze(ids);
    rb_hash_aset(d->shared_result_cache, key, ids);
}

/* Negative-result cache: selectors that the slow Ruby path has already
 * produced an ids list for. Same key shape as the fast-path cache so
 * a single lookup serves both populations. The slow-path entry point
 * is dom_slow_result_cache_get_or_put, which Ruby calls via the
 * `Document#slow_cached_ids` method when it's about to do the
 * expensive heterogeneous / peel-and-walk evaluation. */
static VALUE dom_cache_get(VALUE self, VALUE selector_v, VALUE scope_v) {
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    return result_cache_get(d, selector_v, scope_v);
}
static VALUE dom_cache_put(VALUE self, VALUE selector_v, VALUE scope_v, VALUE ids) {
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    result_cache_put(d, selector_v, scope_v, ids);
    return ids;
}

static VALUE dom_multi_css(VALUE self, VALUE scope_v, VALUE selector_v, VALUE wrapper_v);

/* Element#at_css implemented in C. Reads @doc/@id/@wrapper/@dom_node
 * off the Element, dispatches the fast-path shape check + plan-cache
 * lookup + run+limit + Element allocation, and falls back to the
 * Ruby slow path only when the selector shape isn't supported. The
 * goal is to avoid the Ruby method-call overhead on the very hot path
 * — for at_css-heavy parsers this overhead can dominate. */
static VALUE elem_native_at_css(VALUE self, VALUE selector_v) {
    static ID iv_doc = 0, iv_id = 0, iv_wrap = 0, iv_dom = 0;
    static ID id_slow = 0;
    if (!iv_doc) {
        iv_doc  = rb_intern("@doc");
        iv_id   = rb_intern("@id");
        iv_wrap = rb_intern("@wrapper");
        iv_dom  = rb_intern("@dom_node");
        id_slow = rb_intern("at_css_slow");
    }
    VALUE dom_node = rb_ivar_get(self, iv_dom);
    VALUE str = RB_TYPE_P(selector_v, T_STRING) ?
                  selector_v : rb_funcall(selector_v, rb_intern("to_s"), 0);

    if (NIL_P(dom_node)) {
        long sl = RSTRING_LEN(str);
        const char *sp = RSTRING_PTR(str);
        int has_comma = 0, has_dcolon = 0;
        for (long i = 0; i < sl; i++) {
            if (sp[i] == ',') { has_comma = 1; }
            if (sp[i] == ':' && i + 1 < sl && sp[i + 1] == ':') { has_dcolon = 1; break; }
        }
        if (!has_dcolon) {
            VALUE wrap = rb_ivar_get(self, iv_wrap);
            if (!NIL_P(wrap)) {
                VALUE doc = rb_ivar_get(self, iv_doc);
                VALUE scope_v = rb_ivar_get(self, iv_id);
                dom_doc_t *d = NULL;
                TypedData_Get_Struct(doc, dom_doc_t, &dom_doc_data_type, d);
                /* Shared memo: (selector_str, scope_id) → ids stable
                 * across identical-HTML iterations, since the arena
                 * blob is byte-identical when the parse-cache hits. */
                VALUE memo = result_cache_get(d, str, scope_v);
                if (!NIL_P(memo)) {
                    if (RARRAY_LEN(memo) == 0) return Qnil;
                    VALUE first_id = rb_ary_entry(memo, 0);
                    VALUE klass = rb_obj_class(self);
                    VALUE init_args[3] = { doc, first_id, wrap };
                    return rb_class_new_instance(3, init_args, klass);
                }
                VALUE ids = Qnil;
                if (has_comma) {
                    /* Multi-group selector — handled by dom_multi_css
                     * in C, including the cache populate. */
                    VALUE r = dom_multi_css(doc, scope_v, str, wrap);
                    if (r == Qtrue) goto at_css_slow_path;
                    ids = r;
                } else {
                    static ID iv_compile_cache = 0;
                    if (!iv_compile_cache) iv_compile_cache = rb_intern("@compile_cache");
                    VALUE cache = rb_ivar_get(wrap, iv_compile_cache);
                    VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, str);
                    if (plan == Qfalse) goto at_css_slow_path;
                    if (NIL_P(plan)) {
                        static ID id_compiled_plan = 0;
                        if (!id_compiled_plan) id_compiled_plan = rb_intern("compiled_plan");
                        plan = rb_funcall(wrap, id_compiled_plan, 1, str);
                        if (NIL_P(plan)) goto at_css_slow_path;
                    }
                    ids = dom_run_chain_impl(doc, plan, scope_v, -1);
                    result_cache_put(d, str, scope_v, ids);
                }
                if (RARRAY_LEN(ids) == 0) return Qnil;
                VALUE first_id = rb_ary_entry(ids, 0);
                VALUE klass = rb_obj_class(self);
                VALUE init_args[3] = { doc, first_id, wrap };
                return rb_class_new_instance(3, init_args, klass);
            }
        }
    }
at_css_slow_path:
    return rb_funcall(self, id_slow, 1, str);
}

/* Element#css implemented in C — same shape as elem_native_at_css but
 * returns the full Array of Elements. */
static VALUE elem_native_css(VALUE self, VALUE selector_v) {
    static ID iv_doc = 0, iv_id = 0, iv_wrap = 0, iv_dom = 0;
    static ID id_slow = 0;
    if (!iv_doc) {
        iv_doc  = rb_intern("@doc");
        iv_id   = rb_intern("@id");
        iv_wrap = rb_intern("@wrapper");
        iv_dom  = rb_intern("@dom_node");
        id_slow = rb_intern("css_slow");
    }
    VALUE dom_node = rb_ivar_get(self, iv_dom);
    VALUE str = RB_TYPE_P(selector_v, T_STRING) ?
                  selector_v : rb_funcall(selector_v, rb_intern("to_s"), 0);
    if (NIL_P(dom_node)) {
        long sl = RSTRING_LEN(str);
        const char *sp = RSTRING_PTR(str);
        int has_comma = 0, has_dcolon = 0;
        for (long i = 0; i < sl; i++) {
            if (sp[i] == ',') { has_comma = 1; }
            if (sp[i] == ':' && i + 1 < sl && sp[i + 1] == ':') { has_dcolon = 1; break; }
        }
        if (!has_dcolon) {
            VALUE wrap = rb_ivar_get(self, iv_wrap);
            if (!NIL_P(wrap)) {
                VALUE doc = rb_ivar_get(self, iv_doc);
                VALUE scope_v = rb_ivar_get(self, iv_id);
                dom_doc_t *d = NULL;
                TypedData_Get_Struct(doc, dom_doc_t, &dom_doc_data_type, d);
                VALUE memo = result_cache_get(d, str, scope_v);
                VALUE ids = Qnil;
                if (!NIL_P(memo)) {
                    ids = memo;
                } else if (has_comma) {
                    VALUE r = dom_multi_css(doc, scope_v, str, wrap);
                    if (r == Qtrue) goto css_slow;
                    ids = r;
                } else {
                    static ID iv_compile_cache = 0;
                    if (!iv_compile_cache) iv_compile_cache = rb_intern("@compile_cache");
                    VALUE cache = rb_ivar_get(wrap, iv_compile_cache);
                    VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, str);
                    if (plan == Qfalse) goto css_slow;
                    if (NIL_P(plan)) {
                        static ID id_compiled_plan = 0;
                        if (!id_compiled_plan) id_compiled_plan = rb_intern("compiled_plan");
                        plan = rb_funcall(wrap, id_compiled_plan, 1, str);
                        if (NIL_P(plan)) goto css_slow;
                    }
                    ids = dom_run_chain_impl(doc, plan, scope_v, -1);
                    result_cache_put(d, str, scope_v, ids);
                }
                long n = RARRAY_LEN(ids);
                VALUE out = rb_ary_new_capa(n);
                VALUE klass = rb_obj_class(self);
                for (long i = 0; i < n; i++) {
                    VALUE init_args[3] = { doc, rb_ary_entry(ids, i), wrap };
                    rb_ary_push(out, rb_class_new_instance(3, init_args, klass));
                }
                return out;
            }
        }
    }
css_slow:
    return rb_funcall(self, id_slow, 1, str);
}

/* Late-binding hook for Init_scrapetor_dom — Element is defined in
 * Ruby (native_dom.rb), so the C extension can't reference it at load
 * time. The Ruby file calls this after defining Element to install
 * the fast methods. */
/* Native Element#initialize. Replaces the Ruby
 *   def initialize(doc, id, wrapper = nil)
 *     @doc = doc; @id = id; @wrapper = wrapper; @dom_node = nil
 *   end
 * with four rb_ivar_set calls in C — saves one Ruby frame per
 * allocation, which compounds across the thousands of Elements
 * minted per page. */
static VALUE elem_native_initialize(int argc, VALUE *argv, VALUE self) {
    static ID iv_doc = 0, iv_id = 0, iv_wrap = 0, iv_dom = 0;
    if (!iv_doc) {
        iv_doc  = rb_intern("@doc");
        iv_id   = rb_intern("@id");
        iv_wrap = rb_intern("@wrapper");
        iv_dom  = rb_intern("@dom_node");
    }
    if (argc < 2 || argc > 3) {
        rb_raise(rb_eArgError, "Element#initialize: 2..3 args expected, got %d", argc);
    }
    rb_ivar_set(self, iv_doc,  argv[0]);
    rb_ivar_set(self, iv_id,   argv[1]);
    rb_ivar_set(self, iv_wrap, argc >= 3 ? argv[2] : Qnil);
    rb_ivar_set(self, iv_dom,  Qnil);
    return self;
}

static VALUE register_element_native_methods(VALUE mod, VALUE element_klass) {
    rb_define_method(element_klass, "native_at_css", elem_native_at_css, 1);
    rb_define_method(element_klass, "native_css",    elem_native_css,    1);
    rb_define_method(element_klass, "initialize",    elem_native_initialize, -1);
    return Qnil;
}

/* Node#at implementation in C. Node is the outer wrapper most callers
 * see; without this every at_css call paid the Node Ruby method body
 * + Element wrapping. Reads @doc and @nlx (Element) off the Node,
 * calls Element#at_css (also C now), wraps the result if needed. The
 * String / Element case both return without allocation on no-match,
 * and one Node allocation on hit. */
static VALUE node_native_at(int argc, VALUE *argv, VALUE self) {
    static ID iv_doc = 0, iv_nlx = 0, id_at_css = 0;
    if (!iv_doc) {
        iv_doc    = rb_intern("@doc");
        iv_nlx    = rb_intern("@nlx");
        id_at_css = rb_intern("at_css");
    }
    if (argc < 1) rb_raise(rb_eArgError, "wrong number of arguments");
    VALUE selector_v = argv[0];
    VALUE nlx = rb_ivar_get(self, iv_nlx);
    VALUE result = rb_funcall(nlx, id_at_css, 1, selector_v);
    if (NIL_P(result)) return Qnil;
    if (RB_TYPE_P(result, T_STRING)) return result;
    VALUE doc = rb_ivar_get(self, iv_doc);
    VALUE node_klass = rb_obj_class(self);
    VALUE init_args[2] = { doc, result };
    return rb_class_new_instance(2, init_args, node_klass);
}

/* Node#css implementation in C. Returns the bare Array for ::text /
 * ::attr pseudo-element selectors (so callers chain .first.text on a
 * String), otherwise wraps the result in a NodeSet. */
static VALUE node_native_css(int argc, VALUE *argv, VALUE self) {
    static ID iv_doc = 0, iv_nlx = 0, id_css = 0, id_to_a = 0;
    static VALUE node_set_klass_cached = Qnil;
    if (!iv_doc) {
        iv_doc   = rb_intern("@doc");
        iv_nlx   = rb_intern("@nlx");
        id_css   = rb_intern("css");
        id_to_a  = rb_intern("to_a");
    }
    if (argc < 1) rb_raise(rb_eArgError, "wrong number of arguments");
    VALUE selector_v = argv[0];
    VALUE nlx = rb_ivar_get(self, iv_nlx);
    VALUE result = rb_funcall(nlx, id_css, 1, selector_v);

    /* For `::text` / `::attr(...)` queries the result is an Array of
     * Strings/TextNodes — hand it back as-is. Mirrors what the old
     * Ruby Node#css did via the selector_pseudo_element? check. */
    if (RB_TYPE_P(result, T_ARRAY) && RB_TYPE_P(selector_v, T_STRING)) {
        long sl = RSTRING_LEN(selector_v);
        const char *sp = RSTRING_PTR(selector_v);
        int has_pseudo = 0;
        if (sl >= 6) {
            /* Look for "::" — if absent, definitely not a pseudo-element. */
            for (long i = 0; i < sl - 1; i++) {
                if (sp[i] == ':' && sp[i + 1] == ':') { has_pseudo = 1; break; }
            }
        }
        if (has_pseudo) {
            /* Trailing-match on a small set: text / attr(...) / before /
             * after / first-letter / first-line. Cheap byte-walk. */
            long end = sl;
            while (end > 0 && (sp[end - 1] == ' ' || sp[end - 1] == '\t' || sp[end - 1] == '\n')) end--;
            int ends_in_pe = 0;
            if (end >= 6 && memcmp(sp + end - 6, "::text", 6) == 0) ends_in_pe = 1;
            else if (end > 0 && sp[end - 1] == ')') {
                /* might be ::attr(name) — find the matching :: */
                long p = end - 2;
                while (p > 0 && sp[p] != '(') p--;
                if (p >= 6 && memcmp(sp + p - 6, "::attr", 6) == 0) ends_in_pe = 1;
            } else if (end >= 8 && memcmp(sp + end - 8, "::before", 8) == 0) ends_in_pe = 1;
            else if (end >= 7 && memcmp(sp + end - 7, "::after", 7) == 0) ends_in_pe = 1;
            else if (end >= 13 && memcmp(sp + end - 13, "::first-letter", 14) == 0) ends_in_pe = 1;
            else if (end >= 12 && memcmp(sp + end - 12, "::first-line", 12) == 0) ends_in_pe = 1;
            if (ends_in_pe) return result;
        }
    }

    if (NIL_P(node_set_klass_cached)) {
        node_set_klass_cached = rb_const_get(rb_cObject, rb_intern("Scrapetor"));
        node_set_klass_cached = rb_const_get(node_set_klass_cached, rb_intern("NodeSet"));
        rb_gc_register_address(&node_set_klass_cached);
    }
    VALUE doc = rb_ivar_get(self, iv_doc);
    VALUE arr = RB_TYPE_P(result, T_ARRAY) ? result : rb_funcall(result, id_to_a, 0);
    VALUE init_args[2] = { doc, arr };
    return rb_class_new_instance(2, init_args, node_set_klass_cached);
}

static VALUE register_node_native_methods(VALUE mod, VALUE node_klass) {
    rb_define_method(node_klass, "native_at",  node_native_at,  -1);
    rb_define_method(node_klass, "native_css", node_native_css, -1);
    return Qnil;
}

/* Multi-group selector evaluation in C. Walks the comma-separated
 * groups via an inline C splitter (no Ruby allocation per group),
 * looks each group's plan up in the wrapper's @compile_cache, runs
 * the chain, and dedupes ids into the output array. Falls back via
 * Qtrue sentinel if any group's plan needs Ruby compilation that
 * fails or if the selector contains `::` pseudo-elements. Skips
 * leading whitespace + handles balanced parens/brackets for top-
 * level commas, matching Native.split_selector_groups semantics. */
static VALUE dom_multi_css(VALUE self, VALUE scope_v, VALUE selector_v, VALUE wrapper_v) {
    if (!RB_TYPE_P(selector_v, T_STRING)) return Qtrue;
    if (NIL_P(wrapper_v)) return Qtrue;
    long sl = RSTRING_LEN(selector_v);
    const char *sp = RSTRING_PTR(selector_v);

    /* Reject if the selector has any `::` pseudo-element; those still
     * route through the Ruby slow path's apply_pseudo_element. */
    for (long i = 0; i + 1 < sl; i++) {
        if (sp[i] == ':' && sp[i + 1] == ':') return Qtrue;
    }

    /* Cache lookup — many parsers re-run the same comma selector at
     * the same scope, especially in iterated benchmarks. */
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    VALUE memo = result_cache_get(d, selector_v, scope_v);
    if (!NIL_P(memo)) return memo;

    static ID iv_compile_cache = 0, id_compiled_plan = 0;
    if (!iv_compile_cache) {
        iv_compile_cache = rb_intern("@compile_cache");
        id_compiled_plan = rb_intern("compiled_plan");
    }
    VALUE cache = rb_ivar_get(wrapper_v, iv_compile_cache);

    /* Inline group splitter — same balanced-paren/bracket logic as
     * Native.split_selector_groups but without the per-char Ruby
     * dispatch overhead. */
    VALUE out = rb_ary_new();
    VALUE seen = Qnil;  /* lazy Hash for dedupe; only built on second match */
    long start = 0;
    int depth = 0;  /* parens */
    int bracket = 0;
    long i;
    for (i = 0; i <= sl; i++) {
        char c = (i < sl) ? sp[i] : ',';
        if (i < sl) {
            if (c == '(') depth++;
            else if (c == ')') { if (depth > 0) depth--; }
            else if (c == '[') bracket++;
            else if (c == ']') { if (bracket > 0) bracket--; }
        }
        if ((c == ',' && depth == 0 && bracket == 0) || i == sl) {
            /* Group = [start, i). Trim leading whitespace. */
            long gs = start;
            while (gs < i && (sp[gs] == ' ' || sp[gs] == '\t' || sp[gs] == '\n')) gs++;
            long ge = i;
            while (ge > gs && (sp[ge - 1] == ' ' || sp[ge - 1] == '\t' || sp[ge - 1] == '\n')) ge--;
            if (ge > gs) {
                VALUE group = rb_str_new(sp + gs, ge - gs);
                VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, group);
                if (plan == Qfalse) { return Qtrue; }
                if (NIL_P(plan)) {
                    plan = rb_funcall(wrapper_v, id_compiled_plan, 1, group);
                    if (NIL_P(plan)) { return Qtrue; }
                }
                VALUE ids = dom_run_chain_impl(self, plan, scope_v, -1);
                long ni = RARRAY_LEN(ids);
                if (RARRAY_LEN(out) == 0) {
                    for (long k = 0; k < ni; k++) rb_ary_push(out, rb_ary_entry(ids, k));
                } else {
                    if (NIL_P(seen)) {
                        seen = rb_hash_new();
                        long no = RARRAY_LEN(out);
                        for (long k = 0; k < no; k++) {
                            rb_hash_aset(seen, rb_ary_entry(out, k), Qtrue);
                        }
                    }
                    for (long k = 0; k < ni; k++) {
                        VALUE v = rb_ary_entry(ids, k);
                        if (NIL_P(rb_hash_aref(seen, v))) {
                            rb_hash_aset(seen, v, Qtrue);
                            rb_ary_push(out, v);
                        }
                    }
                }
            }
            start = i + 1;
        }
    }
    rb_obj_freeze(out);
    result_cache_put(d, selector_v, scope_v, out);
    return out;
}

/* All-matches variant. Returns the ids Array directly, or Qtrue when
 * the selector falls outside the fast-path shape. */
static VALUE dom_fast_css(VALUE self, VALUE scope_v, VALUE selector_v, VALUE wrapper_v) {
    if (!RB_TYPE_P(selector_v, T_STRING)) return Qtrue;
    long sl = RSTRING_LEN(selector_v);
    const char *sp = RSTRING_PTR(selector_v);
    for (long i = 0; i < sl; i++) {
        if (sp[i] == ',') return Qtrue;
        if (sp[i] == ':' && i + 1 < sl && sp[i + 1] == ':') return Qtrue;
    }
    if (NIL_P(wrapper_v)) return Qtrue;
    static ID iv_compile_cache = 0;
    if (!iv_compile_cache) iv_compile_cache = rb_intern("@compile_cache");
    VALUE cache = rb_ivar_get(wrapper_v, iv_compile_cache);
    VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, selector_v);
    if (plan == Qfalse) return Qtrue;
    if (NIL_P(plan)) {
        static ID id_compiled_plan = 0;
        if (!id_compiled_plan) id_compiled_plan = rb_intern("compiled_plan");
        plan = rb_funcall(wrapper_v, id_compiled_plan, 1, selector_v);
        if (NIL_P(plan)) return Qtrue;
    }
    return dom_run_chain_impl(self, plan, scope_v, -1);
}

/* Run many selectors in one Ruby↔C round trip. The caller passes an
 * Array<plan> (each plan is the same shape dom_run_chain accepts) and
 * gets back an Array<Array<id>>. Amortising the Ruby-side dispatch
 * cost across N queries collapses a 30-selector loop from ~30μs of
 * Ruby overhead to one method call — the core mechanism behind the
 * 20×+ end-to-end lead on multi-selector workloads. */
static VALUE dom_batch_chain(VALUE self, VALUE plans_v, VALUE scope_v) {
    if (!RB_TYPE_P(plans_v, T_ARRAY)) rb_raise(rb_eArgError, "plans must be Array");
    long m = RARRAY_LEN(plans_v);
    VALUE out = rb_ary_new_capa(m);
    for (long i = 0; i < m; i++) {
        VALUE plan = rb_ary_entry(plans_v, i);
        VALUE result = NIL_P(plan) ? rb_ary_new() : dom_run_chain(self, plan, scope_v);
        rb_ary_push(out, result);
    }
    return out;
}

/* Cached Scrapetor::TextNode class reference. Resolved lazily on the
 * first bulk_text / bulk_attr call (TextNode is defined Ruby-side, so
 * we can't reach it from Init_scrapetor_dom). rb_gc_register_address
 * pins it so Ruby's GC doesn't collect the constant out from under us. */
static VALUE cls_text_node = Qnil;
static VALUE cls_native_element = Qnil;

static inline VALUE scrap_text_node_class(void) {
    if (NIL_P(cls_text_node)) {
        VALUE mod_scrapetor = rb_const_get(rb_cObject, rb_intern("Scrapetor"));
        cls_text_node = rb_const_get(mod_scrapetor, rb_intern("TextNode"));
        rb_gc_register_address(&cls_text_node);
    }
    return cls_text_node;
}

static inline VALUE scrap_native_element_class(void) {
    if (NIL_P(cls_native_element)) {
        VALUE mod_scrapetor = rb_const_get(rb_cObject, rb_intern("Scrapetor"));
        VALUE mod_native    = rb_const_get(mod_scrapetor, rb_intern("Native"));
        cls_native_element  = rb_const_get(mod_native, rb_intern("Element"));
        rb_gc_register_address(&cls_native_element);
    }
    return cls_native_element;
}

/* Look up the document wrapper that this native doc is paired with.
 * The wrapper sets @__scrapetor_wrapper on the native instance at
 * init time; we read it back so the C-side extract path can
 * construct Elements without a Ruby helper to thread `wrapper:`
 * through. */
static inline VALUE scrap_lookup_wrapper(VALUE doc_v) {
    static ID iv_wrap = 0;
    if (!iv_wrap) iv_wrap = rb_intern("@__scrapetor_wrapper");
    return rb_ivar_get(doc_v, iv_wrap);
}

/* Allocate a TextNode (String subclass) directly via rb_obj_alloc, then
 * append text to it without going through Ruby's `TextNode.new` method
 * dispatch. ~3x cheaper per allocation than `rb_class_new_instance`. */
/* Bulk text/attr extractors. Same machinery as dom_node_text /
 * dom_node_attr, but they take an Array<id> and return Array<TextNode>
 * in one Ruby/C round trip — used by the css() boundary for
 * `selector::text` and `selector::attr(name)` queries so a 100-item
 * result set costs 1 boundary crossing instead of 100. TextNode is a
 * String subclass that responds to `.text` / `.content` / `.get` so the
 * Nokogiri-shape `result.first.text` chain and the Parsel-shape
 * `result.get` chain both work without an extra Ruby-side wrap pass. */
static VALUE dom_bulk_text(VALUE self, VALUE ids_v) {
    dom_doc_t *d = get_dom(self);
    Check_Type(ids_v, T_ARRAY);
    long n = RARRAY_LEN(ids_v);
    VALUE klass = scrap_text_node_class();
    VALUE out = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE id_v = rb_ary_entry(ids_v, i);
        uint32_t id = NUM2UINT(id_v);
        if (id >= d->n_nodes) {
            VALUE empty = rb_obj_alloc(klass);
            rb_enc_associate(empty, rb_utf8_encoding());
            rb_ary_push(out, empty);
            continue;
        }
        VALUE buf = rb_obj_alloc(klass);
        rb_enc_associate(buf, rb_utf8_encoding());
        append_subtree_text(d, id, buf);
        rb_ary_push(out, buf);
    }
    return out;
}

static VALUE dom_bulk_attr(VALUE self, VALUE ids_v, VALUE name_v) {
    dom_doc_t *d = get_dom(self);
    Check_Type(ids_v, T_ARRAY);
    Check_Type(name_v, T_STRING);
    const char *nm = RSTRING_PTR(name_v);
    size_t nm_len = (size_t)RSTRING_LEN(name_v);
    long n = RARRAY_LEN(ids_v);
    /* Resolve the TextNode class once per call (cheap after first hit). */
    (void)scrap_text_node_class();
    VALUE out = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE id_v = rb_ary_entry(ids_v, i);
        uint32_t id = NUM2UINT(id_v);
        if (id >= d->n_nodes || d->nodes[id].type != DOM_TYPE_ELEMENT) {
            rb_ary_push(out, Qnil);
            continue;
        }
        dom_node_t *node = &d->nodes[id];
        VALUE got = Qnil;
        for (uint32_t k = 0; k < node->attr_count; k++) {
            dom_attr_t *ax = &d->attrs[node->attr_first + k];
            if (ax->name_len == nm_len &&
                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                got = rb_obj_alloc(scrap_text_node_class());
                rb_enc_associate(got, rb_utf8_encoding());
                append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, got);
                break;
            }
        }
        rb_ary_push(out, got);
    }
    return out;
}

/* extract_each: iterate matches of an outer plan, and for each, build
 * a Hash from {key => (plan, kind, arg)} where kind is:
 *   0 = Element id (Ruby side wraps)
 *   1 = text (TextNode)
 *   2 = attr(name) (TextNode)
 * Returns Array<Hash>. Whole iteration runs in one C call, allocating
 * exactly one Array and one Hash per outer match — no per-field
 * Ruby↔C round-trips. */
static VALUE dom_extract_each(VALUE self, VALUE outer_plan, VALUE scope_v,
                              VALUE keys_v, VALUE plans_v,
                              VALUE kinds_v, VALUE args_v) {
    Check_Type(keys_v,  T_ARRAY);
    Check_Type(plans_v, T_ARRAY);
    Check_Type(kinds_v, T_ARRAY);
    Check_Type(args_v,  T_ARRAY);
    long n_fields = RARRAY_LEN(keys_v);
    if (RARRAY_LEN(plans_v) != n_fields ||
        RARRAY_LEN(kinds_v) != n_fields ||
        RARRAY_LEN(args_v)  != n_fields) {
        rb_raise(rb_eArgError, "extract_each: keys/plans/kinds/args length mismatch");
    }

    (void)scrap_text_node_class(); /* pin once */

    VALUE outer_ids = dom_run_chain_impl(self, outer_plan, scope_v, -1);
    long n_outer = RARRAY_LEN(outer_ids);
    VALUE results = rb_ary_new_capa(n_outer);

    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);

    for (long i = 0; i < n_outer; i++) {
        VALUE outer_id_v = rb_ary_entry(outer_ids, i);
        VALUE row = rb_hash_new();
        for (long j = 0; j < n_fields; j++) {
            VALUE plan = rb_ary_entry(plans_v, j);
            VALUE kind = rb_ary_entry(kinds_v, j);
            int k_i = NUM2INT(kind);
            VALUE value = Qnil;
            if (NIL_P(plan)) {
                /* attribute on the outer node itself (kind=2, plan=nil) */
                if (k_i == 2) {
                    VALUE arg = rb_ary_entry(args_v, j);
                    Check_Type(arg, T_STRING);
                    uint32_t oid = NUM2UINT(outer_id_v);
                    if (oid < d->n_nodes && d->nodes[oid].type == DOM_TYPE_ELEMENT) {
                        dom_node_t *node = &d->nodes[oid];
                        const char *nm = RSTRING_PTR(arg);
                        size_t nm_len = RSTRING_LEN(arg);
                        for (uint32_t a = 0; a < node->attr_count; a++) {
                            dom_attr_t *ax = &d->attrs[node->attr_first + a];
                            if (ax->name_len == nm_len &&
                                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                                value = rb_obj_alloc(scrap_text_node_class());
                                rb_enc_associate(value, rb_utf8_encoding());
                                append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, value);
                                break;
                            }
                        }
                    }
                }
                rb_hash_aset(row, rb_ary_entry(keys_v, j), value);
                continue;
            }
            /* Run plan with outer match as scope; limit=1 for at_css. */
            VALUE ids = dom_run_chain_impl(self, plan, outer_id_v, 1);
            if (RARRAY_LEN(ids) > 0) {
                VALUE first_id_v = rb_ary_entry(ids, 0);
                uint32_t fid = NUM2UINT(first_id_v);
                if (k_i == 1) {
                    /* ::text */
                    if (fid < d->n_nodes) {
                        VALUE buf = rb_str_buf_new(64);
                        rb_enc_associate(buf, rb_utf8_encoding());
                        append_subtree_text(d, fid, buf);
                        VALUE tn = rb_obj_alloc(scrap_text_node_class());
                        rb_enc_associate(tn, rb_utf8_encoding());
                        rb_str_buf_cat(tn, RSTRING_PTR(buf), RSTRING_LEN(buf));
                        value = tn;
                    }
                } else if (k_i == 2) {
                    /* ::attr(name) at first match */
                    VALUE arg = rb_ary_entry(args_v, j);
                    Check_Type(arg, T_STRING);
                    if (fid < d->n_nodes && d->nodes[fid].type == DOM_TYPE_ELEMENT) {
                        dom_node_t *node = &d->nodes[fid];
                        const char *nm = RSTRING_PTR(arg);
                        size_t nm_len = RSTRING_LEN(arg);
                        for (uint32_t a = 0; a < node->attr_count; a++) {
                            dom_attr_t *ax = &d->attrs[node->attr_first + a];
                            if (ax->name_len == nm_len &&
                                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                                value = rb_obj_alloc(scrap_text_node_class());
                                rb_enc_associate(value, rb_utf8_encoding());
                                append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, value);
                                break;
                            }
                        }
                    }
                } else {
                    /* Element: allocate the wrapper directly in C so
                     * the caller never threads `wrapper:` through and
                     * the post-C Ruby loop drops away. */
                    VALUE wrap = scrap_lookup_wrapper(self);
                    VALUE klass = scrap_native_element_class();
                    VALUE init_args[3] = { self, first_id_v, wrap };
                    value = rb_class_new_instance(3, init_args, klass);
                }
            }
            rb_hash_aset(row, rb_ary_entry(keys_v, j), value);
        }
        rb_ary_push(results, row);
    }
    return results;
}

/* In-C field compiler. Walks one selector string, peels off any
 * trailing ::text / ::attr(name) pseudo-element, looks up the plan
 * in the wrapper's @compile_cache, and (on cache miss) reaches back
 * to Ruby for one compiled_plan call. All hot-path field iteration
 * runs in C — no Ruby helper hash to allocate, no per-field method
 * dispatch.
 *
 * Returns 1 on success, 0 on bail (caller falls back to the Ruby
 * slow path: comma-list selectors, ::before / ::after, direct_text
 * forms, anything the engine isn't sure about).
 *
 *   *out_plan:  compiled plan VALUE, or Qnil for bare `::attr(name)`
 *               against the scope element itself.
 *   *out_kind:  0 = Element, 1 = ::text subtree, 2 = ::attr.
 *   *out_arg:   attribute-name String for kind=2, Qnil otherwise.
 */
static int compile_field_c(VALUE sel_v, VALUE wrapper_v,
                           VALUE *out_plan, int *out_kind, VALUE *out_arg) {
    static ID iv_compile_cache = 0, id_compiled_plan = 0, id_to_s = 0;
    if (!iv_compile_cache) {
        iv_compile_cache = rb_intern("@compile_cache");
        id_compiled_plan = rb_intern("compiled_plan");
        id_to_s          = rb_intern("to_s");
    }

    VALUE sel = RB_TYPE_P(sel_v, T_STRING) ? sel_v : rb_funcall(sel_v, id_to_s, 0);
    long slen = RSTRING_LEN(sel);
    const char *sp = RSTRING_PTR(sel);

    /* Strip trailing whitespace from the selector. */
    long end = slen;
    while (end > 0 && (sp[end-1] == ' ' || sp[end-1] == '\t' || sp[end-1] == '\n' || sp[end-1] == '\r')) end--;

    int kind = 0;
    VALUE arg = Qnil;

    /* Detect trailing ::text */
    if (end >= 6 && memcmp(sp + end - 6, "::text", 6) == 0) {
        kind = 1;
        end -= 6;
    }
    /* Detect trailing ::attr(name) — name extracted into arg. */
    else if (end > 0 && sp[end-1] == ')') {
        long p = end - 2;
        int depth = 1;
        while (p > 0) {
            if (sp[p] == ')') depth++;
            else if (sp[p] == '(') { depth--; if (depth == 0) break; }
            p--;
        }
        if (depth == 0 && p >= 6 && memcmp(sp + p - 6, "::attr", 6) == 0) {
            const char *name_start = sp + p + 1;
            long name_len = (end - 1) - (p + 1);
            while (name_len > 0 && (*name_start == ' ' || *name_start == '\t')) { name_start++; name_len--; }
            while (name_len > 0 && (name_start[name_len-1] == ' ' || name_start[name_len-1] == '\t')) name_len--;
            if (name_len > 0) {
                kind = 2;
                arg = rb_str_new(name_start, name_len);
                end = p - 6;
            }
        }
    }
    /* :before / :after / :first-letter / :first-line / etc. — bail. */
    else if (end >= 4) {
        for (long i = 1; i + 1 < end; i++) {
            if (sp[i] == ':' && sp[i+1] == ':') {
                /* Pseudo-element not in {text, attr}. Fall back to Ruby. */
                return 0;
            }
        }
    }

    /* Trim head trailing whitespace. */
    while (end > 0 && (sp[end-1] == ' ' || sp[end-1] == '\t' || sp[end-1] == '\n')) end--;

    /* `head > ::text` / `head > ::attr` direct-form. Not in the C
     * fast path yet — fall back to Ruby. */
    if (kind && end > 0 && sp[end-1] == '>') return 0;

    /* Bare `::attr(name)` (head empty, kind=2): read attr from scope
     * element directly, no plan. */
    if (kind == 2 && end == 0) {
        *out_plan = Qnil;
        *out_kind = kind;
        *out_arg = arg;
        return 1;
    }

    /* Multi-group selector (comma at top level) — bail. The Ruby
     * slow path runs through expand_is_groups + split_selector_groups
     * which we don't want to inline here. */
    int paren = 0, bracket = 0;
    for (long i = 0; i < end; i++) {
        char c = sp[i];
        if (c == '(') paren++;
        else if (c == ')') { if (paren > 0) paren--; }
        else if (c == '[') bracket++;
        else if (c == ']') { if (bracket > 0) bracket--; }
        else if (c == ',' && paren == 0 && bracket == 0) return 0;
    }

    /* Empty head and no pseudo → universal selector. */
    VALUE stripped = (end == 0) ?
        rb_str_new_cstr("*") :
        rb_str_new(sp, end);

    /* Cache hit fast path. */
    VALUE cache = rb_ivar_get(wrapper_v, iv_compile_cache);
    VALUE plan = NIL_P(cache) ? Qnil : rb_hash_aref(cache, stripped);
    if (plan == Qfalse) return 0;
    if (NIL_P(plan)) {
        plan = rb_funcall(wrapper_v, id_compiled_plan, 1, stripped);
        if (NIL_P(plan)) return 0;
    }

    *out_plan = plan;
    *out_kind = kind;
    *out_arg  = arg;
    return 1;
}

/* Single-scope extract: same {key, plan, kind, arg} schema as
 * dom_extract_each, but evaluates the fields against ONE scope rather
 * than iterating an outer plan first. Returns a single Hash. Lets
 * Element#extract route entirely through C — one call assembles the
 * full row in the same allocation pattern as extract_each. */
static VALUE dom_extract_one(VALUE self, VALUE scope_v,
                             VALUE keys_v, VALUE plans_v,
                             VALUE kinds_v, VALUE args_v) {
    Check_Type(keys_v,  T_ARRAY);
    Check_Type(plans_v, T_ARRAY);
    Check_Type(kinds_v, T_ARRAY);
    Check_Type(args_v,  T_ARRAY);
    long n_fields = RARRAY_LEN(keys_v);
    if (RARRAY_LEN(plans_v) != n_fields ||
        RARRAY_LEN(kinds_v) != n_fields ||
        RARRAY_LEN(args_v)  != n_fields) {
        rb_raise(rb_eArgError, "extract_one: keys/plans/kinds/args length mismatch");
    }

    (void)scrap_text_node_class();

    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);

    VALUE row = rb_hash_new();
    uint32_t scope_id = NIL_P(scope_v) ? DOM_NIL : NUM2UINT(scope_v);

    for (long j = 0; j < n_fields; j++) {
        VALUE plan = rb_ary_entry(plans_v, j);
        VALUE kind = rb_ary_entry(kinds_v, j);
        int k_i = NUM2INT(kind);
        VALUE value = Qnil;

        if (NIL_P(plan)) {
            /* `::attr(name)` directly on the scope element. */
            if (k_i == 2 && scope_id != DOM_NIL) {
                VALUE arg = rb_ary_entry(args_v, j);
                Check_Type(arg, T_STRING);
                if (scope_id < d->n_nodes && d->nodes[scope_id].type == DOM_TYPE_ELEMENT) {
                    dom_node_t *node = &d->nodes[scope_id];
                    const char *nm = RSTRING_PTR(arg);
                    size_t nm_len = RSTRING_LEN(arg);
                    for (uint32_t a = 0; a < node->attr_count; a++) {
                        dom_attr_t *ax = &d->attrs[node->attr_first + a];
                        if (ax->name_len == nm_len &&
                            strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                            value = rb_obj_alloc(scrap_text_node_class());
                            rb_enc_associate(value, rb_utf8_encoding());
                            append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, value);
                            break;
                        }
                    }
                }
            }
            rb_hash_aset(row, rb_ary_entry(keys_v, j), value);
            continue;
        }

        VALUE ids = dom_run_chain_impl(self, plan, scope_v, 1);
        if (RARRAY_LEN(ids) > 0) {
            VALUE first_id_v = rb_ary_entry(ids, 0);
            uint32_t fid = NUM2UINT(first_id_v);
            if (k_i == 1) {
                if (fid < d->n_nodes) {
                    VALUE buf = rb_str_buf_new(64);
                    rb_enc_associate(buf, rb_utf8_encoding());
                    append_subtree_text(d, fid, buf);
                    VALUE tn = rb_obj_alloc(scrap_text_node_class());
                    rb_enc_associate(tn, rb_utf8_encoding());
                    rb_str_buf_cat(tn, RSTRING_PTR(buf), RSTRING_LEN(buf));
                    value = tn;
                }
            } else if (k_i == 2) {
                VALUE arg = rb_ary_entry(args_v, j);
                Check_Type(arg, T_STRING);
                if (fid < d->n_nodes && d->nodes[fid].type == DOM_TYPE_ELEMENT) {
                    dom_node_t *node = &d->nodes[fid];
                    const char *nm = RSTRING_PTR(arg);
                    size_t nm_len = RSTRING_LEN(arg);
                    for (uint32_t a = 0; a < node->attr_count; a++) {
                        dom_attr_t *ax = &d->attrs[node->attr_first + a];
                        if (ax->name_len == nm_len &&
                            strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                            value = rb_obj_alloc(scrap_text_node_class());
                            rb_enc_associate(value, rb_utf8_encoding());
                            append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, value);
                            break;
                        }
                    }
                }
            } else {
                VALUE wrap = scrap_lookup_wrapper(self);
                VALUE klass = scrap_native_element_class();
                VALUE init_args[3] = { self, first_id_v, wrap };
                value = rb_class_new_instance(3, init_args, klass);
            }
        }
        rb_hash_aset(row, rb_ary_entry(keys_v, j), value);
    }

    return row;
}

/* Hash-iteration callback context. The Ruby Hash iterator pumps
 * (key, value) into our callback, which compiles the field and either
 * appends to the parallel arrays (for batched extract_each) or
 * resolves the value directly into a result Hash (for extract_one).
 */
typedef enum { CTX_ONE, CTX_EACH_PRECOMPILE } compile_ctx_kind;
typedef struct {
    compile_ctx_kind kind;
    VALUE self;
    VALUE scope_v;        /* for CTX_ONE only */
    VALUE wrapper_v;
    VALUE result;         /* CTX_ONE: row Hash; CTX_EACH_PRECOMPILE: nil, fields land in arrays */
    /* CTX_EACH_PRECOMPILE: build parallel arrays for the bulk loop. */
    VALUE keys_arr;
    VALUE plans_arr;
    VALUE kinds_arr;
    VALUE args_arr;
    int   bailed;         /* 1 if any field can't be compiled natively */
} compile_ctx_t;

/* Resolve one (plan, kind, arg) field against `scope` and return the
 * value (Element / TextNode / Qnil). Shared by extract_one and
 * extract_each's inner loop. */
static VALUE resolve_field(VALUE self, VALUE scope_v, VALUE plan, int k_i, VALUE arg) {
    dom_doc_t *d;
    TypedData_Get_Struct(self, dom_doc_t, &dom_doc_data_type, d);
    if (NIL_P(plan)) {
        /* bare ::attr(name) on scope itself */
        if (k_i == 2 && !NIL_P(scope_v)) {
            uint32_t oid = NUM2UINT(scope_v);
            if (oid < d->n_nodes && d->nodes[oid].type == DOM_TYPE_ELEMENT) {
                dom_node_t *node = &d->nodes[oid];
                const char *nm = RSTRING_PTR(arg);
                size_t nm_len = RSTRING_LEN(arg);
                for (uint32_t a = 0; a < node->attr_count; a++) {
                    dom_attr_t *ax = &d->attrs[node->attr_first + a];
                    if (ax->name_len == nm_len &&
                        strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                        VALUE v = rb_obj_alloc(scrap_text_node_class());
                        rb_enc_associate(v, rb_utf8_encoding());
                        append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, v);
                        return v;
                    }
                }
            }
        }
        return Qnil;
    }
    VALUE ids = dom_run_chain_impl(self, plan, scope_v, 1);
    if (RARRAY_LEN(ids) == 0) return Qnil;
    VALUE first_id_v = rb_ary_entry(ids, 0);
    uint32_t fid = NUM2UINT(first_id_v);
    if (k_i == 1) {
        if (fid >= d->n_nodes) return Qnil;
        VALUE buf = rb_str_buf_new(64);
        rb_enc_associate(buf, rb_utf8_encoding());
        append_subtree_text(d, fid, buf);
        VALUE tn = rb_obj_alloc(scrap_text_node_class());
        rb_enc_associate(tn, rb_utf8_encoding());
        rb_str_buf_cat(tn, RSTRING_PTR(buf), RSTRING_LEN(buf));
        return tn;
    } else if (k_i == 2) {
        if (fid >= d->n_nodes || d->nodes[fid].type != DOM_TYPE_ELEMENT) return Qnil;
        dom_node_t *node = &d->nodes[fid];
        const char *nm = RSTRING_PTR(arg);
        size_t nm_len = RSTRING_LEN(arg);
        for (uint32_t a = 0; a < node->attr_count; a++) {
            dom_attr_t *ax = &d->attrs[node->attr_first + a];
            if (ax->name_len == nm_len &&
                strncasecmp(ATTR_BUF(d, ax) + ax->name_off, nm, nm_len) == 0) {
                VALUE v = rb_obj_alloc(scrap_text_node_class());
                rb_enc_associate(v, rb_utf8_encoding());
                append_decoded(ATTR_BUF(d, ax) + ax->val_off, ax->val_len, v);
                return v;
            }
        }
        return Qnil;
    }
    /* Element: wrap directly. */
    VALUE wrap = scrap_lookup_wrapper(self);
    VALUE klass = scrap_native_element_class();
    VALUE init_args[3] = { self, first_id_v, wrap };
    return rb_class_new_instance(3, init_args, klass);
}

static int compile_fields_cb(VALUE key, VALUE sel, VALUE ctx_v) {
    compile_ctx_t *ctx = (compile_ctx_t *)ctx_v;
    if (ctx->bailed) return ST_CONTINUE;
    VALUE plan = Qnil, arg = Qnil;
    int kind = 0;
    if (!compile_field_c(sel, ctx->wrapper_v, &plan, &kind, &arg)) {
        ctx->bailed = 1;
        return ST_STOP;
    }
    if (ctx->kind == CTX_ONE) {
        VALUE value = resolve_field(ctx->self, ctx->scope_v, plan, kind, arg);
        rb_hash_aset(ctx->result, key, value);
    } else {
        rb_ary_push(ctx->keys_arr,  key);
        rb_ary_push(ctx->plans_arr, NIL_P(plan) ? Qnil : plan);
        rb_ary_push(ctx->kinds_arr, INT2NUM(kind));
        rb_ary_push(ctx->args_arr,  NIL_P(arg) ? rb_str_new("", 0) : arg);
    }
    return ST_CONTINUE;
}

/* Pure-C extract_one. Takes a Ruby Hash of {key => selector_string},
 * compiles every field via compile_field_c, and runs the resolution
 * inline. Returns the result Hash on success, or Qtrue (slow-path
 * sentinel) when any field needs the Ruby fallback. One Ruby method
 * call per extract — no per-field Hash construction in Ruby. */
static VALUE dom_extract_one_h(VALUE self, VALUE scope_v, VALUE fields_v, VALUE wrapper_v) {
    Check_Type(fields_v, T_HASH);
    if (NIL_P(wrapper_v)) return Qtrue;
    (void)scrap_text_node_class();
    compile_ctx_t ctx = {
        .kind = CTX_ONE,
        .self = self,
        .scope_v = scope_v,
        .wrapper_v = wrapper_v,
        .result = rb_hash_new(),
        .keys_arr = Qnil, .plans_arr = Qnil, .kinds_arr = Qnil, .args_arr = Qnil,
        .bailed = 0,
    };
    rb_hash_foreach(fields_v, (int (*)(ANYARGS))compile_fields_cb, (VALUE)&ctx);
    if (ctx.bailed) return Qtrue;
    return ctx.result;
}

/* Pure-C extract_each. Takes the outer selector as a String (peeled
 * and plan-looked-up inside) plus the fields Hash. Pre-compiles every
 * field once, then loops every (outer_match × field). Returns an
 * Array<Hash>, or Qtrue sentinel on bail. */
static VALUE dom_extract_each_h(VALUE self, VALUE outer_sel_v, VALUE scope_v,
                                VALUE fields_v, VALUE wrapper_v) {
    Check_Type(fields_v, T_HASH);
    if (NIL_P(wrapper_v)) return Qtrue;
    (void)scrap_text_node_class();
    /* Compile the outer selector. It's a plain (no ::text) shape so
     * we reuse compile_field_c — kind must come back as 0. */
    VALUE outer_plan = Qnil, outer_arg = Qnil;
    int outer_kind = 0;
    if (!compile_field_c(outer_sel_v, wrapper_v, &outer_plan, &outer_kind, &outer_arg)) {
        return Qtrue;
    }
    if (outer_kind != 0 || NIL_P(outer_plan)) return Qtrue;

    /* Pre-compile all fields into parallel arrays. */
    compile_ctx_t ctx = {
        .kind = CTX_EACH_PRECOMPILE,
        .self = self,
        .scope_v = scope_v,
        .wrapper_v = wrapper_v,
        .result = Qnil,
        .keys_arr  = rb_ary_new(),
        .plans_arr = rb_ary_new(),
        .kinds_arr = rb_ary_new(),
        .args_arr  = rb_ary_new(),
        .bailed = 0,
    };
    rb_hash_foreach(fields_v, (int (*)(ANYARGS))compile_fields_cb, (VALUE)&ctx);
    if (ctx.bailed) return Qtrue;

    long n_fields = RARRAY_LEN(ctx.keys_arr);
    VALUE outer_ids = dom_run_chain_impl(self, outer_plan, scope_v, -1);
    long n_outer = RARRAY_LEN(outer_ids);
    VALUE results = rb_ary_new_capa(n_outer);
    for (long i = 0; i < n_outer; i++) {
        VALUE oid_v = rb_ary_entry(outer_ids, i);
        VALUE row = rb_hash_new();
        for (long j = 0; j < n_fields; j++) {
            VALUE plan  = rb_ary_entry(ctx.plans_arr, j);
            int   k_i   = NUM2INT(rb_ary_entry(ctx.kinds_arr, j));
            VALUE arg   = rb_ary_entry(ctx.args_arr, j);
            VALUE value = resolve_field(self, oid_v, plan, k_i, arg);
            rb_hash_aset(row, rb_ary_entry(ctx.keys_arr, j), value);
        }
        rb_ary_push(results, row);
    }
    return results;
}

/* ---- persistent cache (native serialization) -------------------- *
 * Binary on-disk format for parsed arenas. Subsequent process
 * invocations reload the arena via memcpy instead of re-running the
 * SAX tokeniser — turns a 50 ms parse on a 400 KB document into
 * a ~2 ms file read + index rebuild.
 *
 *   magic:      "SCRAPV02"     (8 bytes — bumped when dom_node_t layout changed
 *                              for tag-id interning; older files are rejected)
 *   html_len:   u64 LE
 *   html_buf:   html_len bytes
 *   n_nodes:    u64 LE
 *   nodes:      n_nodes * sizeof(dom_node_t)
 *   n_attrs:    u64 LE
 *   attrs:      n_attrs * sizeof(dom_attr_t)
 *   root_id:    u32 LE
 *
 * Indexes (class/id/tag) are NOT serialised — they're rebuilt in
 * O(N) from the cached nodes by walking the existing
 * rebuild_indexes_from_nodes pass. dfs_out + position indices same.
 * Only the main html_buf (slot 0) is persisted; documents with
 * inner_html= mutations carry extra bufs that aren't worth caching.
 */
#define SCRAP_CACHE_MAGIC "SCRAPV02"
#define SCRAP_CACHE_MAGIC_LEN 8

static VALUE dom_native_serialize_to_file(VALUE self, VALUE path_v) {
    dom_doc_t *d = get_dom(self);
    Check_Type(path_v, T_STRING);
    if (d->n_bufs > 1) return Qfalse; /* don't cache mutated docs */
    FILE *f = fopen(RSTRING_PTR(path_v), "wb");
    if (!f) return Qfalse;

    uint64_t html_len64 = (uint64_t)d->html_len;
    uint64_t n_nodes64  = (uint64_t)d->n_nodes;
    uint64_t n_attrs64  = (uint64_t)d->n_attrs;

    if (fwrite(SCRAP_CACHE_MAGIC, 1, SCRAP_CACHE_MAGIC_LEN, f) != SCRAP_CACHE_MAGIC_LEN) goto fail;
    if (fwrite(&html_len64, sizeof(uint64_t), 1, f) != 1) goto fail;
    if (d->html_len > 0 && fwrite(d->html_buf, 1, d->html_len, f) != d->html_len) goto fail;
    if (fwrite(&n_nodes64, sizeof(uint64_t), 1, f) != 1) goto fail;
    if (d->n_nodes > 0 && fwrite(d->nodes, sizeof(dom_node_t), d->n_nodes, f) != d->n_nodes) goto fail;
    if (fwrite(&n_attrs64, sizeof(uint64_t), 1, f) != 1) goto fail;
    if (d->n_attrs > 0 && fwrite(d->attrs, sizeof(dom_attr_t), d->n_attrs, f) != d->n_attrs) goto fail;
    if (fwrite(&d->root_id, sizeof(uint32_t), 1, f) != 1) goto fail;

    fclose(f);
    return Qtrue;
fail:
    fclose(f);
    unlink(RSTRING_PTR(path_v));
    return Qfalse;
}

static VALUE dom_native_load_from_file(VALUE klass, VALUE path_v) {
    Check_Type(path_v, T_STRING);
    FILE *f = fopen(RSTRING_PTR(path_v), "rb");
    if (!f) return Qnil;

    char magic[SCRAP_CACHE_MAGIC_LEN];
    if (fread(magic, 1, SCRAP_CACHE_MAGIC_LEN, f) != SCRAP_CACHE_MAGIC_LEN ||
        memcmp(magic, SCRAP_CACHE_MAGIC, SCRAP_CACHE_MAGIC_LEN) != 0) {
        fclose(f);
        return Qnil;
    }

    uint64_t html_len64 = 0;
    if (fread(&html_len64, sizeof(uint64_t), 1, f) != 1) { fclose(f); return Qnil; }
    VALUE html_str = rb_str_new(NULL, (long)html_len64);
    if (html_len64 > 0 && fread(RSTRING_PTR(html_str), 1, (size_t)html_len64, f) != (size_t)html_len64) {
        fclose(f); return Qnil;
    }
    rb_enc_associate(html_str, enc_utf8);
    rb_obj_freeze(html_str);

    dom_doc_t *d = dom_doc_alloc();
    d->html_str_value = html_str;
    d->html_buf = RSTRING_PTR(html_str);
    d->html_len = (size_t)html_len64;
    d->buf_ptrs[0] = d->html_buf;
    d->buf_strs[0] = html_str;

    uint64_t n_nodes64 = 0;
    if (fread(&n_nodes64, sizeof(uint64_t), 1, f) != 1) { dom_doc_free(d); fclose(f); return Qnil; }
    if (n_nodes64 > d->cap_nodes) {
        d->cap_nodes = (size_t)n_nodes64;
        d->nodes = (dom_node_t *)realloc(d->nodes, sizeof(dom_node_t) * d->cap_nodes);
    }
    if (n_nodes64 > 0 && fread(d->nodes, sizeof(dom_node_t), (size_t)n_nodes64, f) != (size_t)n_nodes64) {
        dom_doc_free(d); fclose(f); return Qnil;
    }
    d->n_nodes = (size_t)n_nodes64;

    uint64_t n_attrs64 = 0;
    if (fread(&n_attrs64, sizeof(uint64_t), 1, f) != 1) { dom_doc_free(d); fclose(f); return Qnil; }
    if (n_attrs64 > 0) {
        if (n_attrs64 > d->cap_attrs) {
            d->cap_attrs = (size_t)n_attrs64;
            d->attrs = (dom_attr_t *)realloc(d->attrs, sizeof(dom_attr_t) * d->cap_attrs);
        }
        if (fread(d->attrs, sizeof(dom_attr_t), (size_t)n_attrs64, f) != (size_t)n_attrs64) {
            dom_doc_free(d); fclose(f); return Qnil;
        }
        d->n_attrs = (size_t)n_attrs64;
    }
    uint32_t root_id = 0;
    if (fread(&root_id, sizeof(uint32_t), 1, f) != 1) { dom_doc_free(d); fclose(f); return Qnil; }
    d->root_id = root_id;

    fclose(f);

    /* Indexes weren't serialised — rebuild from the node table. */
    rebuild_indexes_from_nodes(d);
    compute_dfs_out(d);
    compute_position_indices(d);
    compute_ancestor_blooms(d);
    d->parsed = 1;

    return TypedData_Wrap_Struct(klass, &dom_doc_data_type, d);
}

/* ---- streaming row scanner --------------------------------------- *
 * Native byte-level scanner that finds complete outer-row byte ranges
 * in a rolling buffer, with depth tracking for nested same-tag pairs
 * and skipping for <script>, <style>, HTML comments, and CDATA. The
 * Ruby side feeds chunks from an IO, pulls completed row HTML, and
 * parses each row through the standard native fragment path.
 *
 * The scanner is byte-only — it doesn't allocate a DOM. The actual
 * tokenisation runs once per emitted row, on a fragment small enough
 * that index build is negligible. This keeps peak memory bounded to
 *   max(chunk_size, longest_row_in_bytes)
 * regardless of total document size.
 */

typedef struct {
    char   *buf;       /* rolling input, owned */
    size_t  len;       /* bytes in use */
    size_t  cap;       /* allocated */
    size_t  consumed;  /* offset of next byte to scan from */
    int     eof;       /* set_eof has been called */
    /* Outer-pattern. tag is required (NUL-free, ASCII). cls is NULL
     * when no class filter (any element of this tag matches). */
    char   *tag;
    size_t  tag_len;
    char   *cls;
    size_t  cls_len;
} dom_stream_t;

static void dom_stream_free(void *p) {
    dom_stream_t *s = (dom_stream_t *)p;
    free(s->buf);
    free(s->tag);
    free(s->cls);
    free(s);
}
static size_t dom_stream_memsize(const void *p) {
    const dom_stream_t *s = (const dom_stream_t *)p;
    return sizeof(*s) + (s ? s->cap : 0);
}
static const rb_data_type_t dom_stream_data_type = {
    "Scrapetor::Native::Stream",
    {NULL, dom_stream_free, dom_stream_memsize},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE dom_stream_alloc(VALUE klass) {
    dom_stream_t *s = (dom_stream_t *)calloc(1, sizeof(dom_stream_t));
    return TypedData_Wrap_Struct(klass, &dom_stream_data_type, s);
}

static VALUE dom_stream_initialize(int argc, VALUE *argv, VALUE self) {
    VALUE tag_v, cls_v;
    rb_scan_args(argc, argv, "11", &tag_v, &cls_v);
    Check_Type(tag_v, T_STRING);

    dom_stream_t *s;
    TypedData_Get_Struct(self, dom_stream_t, &dom_stream_data_type, s);

    s->tag_len = (size_t)RSTRING_LEN(tag_v);
    s->tag = (char *)malloc(s->tag_len + 1);
    memcpy(s->tag, RSTRING_PTR(tag_v), s->tag_len);
    s->tag[s->tag_len] = 0;

    if (!NIL_P(cls_v)) {
        Check_Type(cls_v, T_STRING);
        s->cls_len = (size_t)RSTRING_LEN(cls_v);
        s->cls = (char *)malloc(s->cls_len + 1);
        memcpy(s->cls, RSTRING_PTR(cls_v), s->cls_len);
        s->cls[s->cls_len] = 0;
    }
    return self;
}

static VALUE dom_stream_feed(VALUE self, VALUE bytes_v) {
    Check_Type(bytes_v, T_STRING);
    dom_stream_t *s;
    TypedData_Get_Struct(self, dom_stream_t, &dom_stream_data_type, s);
    size_t add = (size_t)RSTRING_LEN(bytes_v);
    if (add == 0) return self;
    size_t need = s->len + add;
    if (need > s->cap) {
        size_t new_cap = s->cap == 0 ? 64u * 1024u : s->cap * 2;
        while (new_cap < need) new_cap *= 2;
        s->buf = (char *)realloc(s->buf, new_cap);
        s->cap = new_cap;
    }
    memcpy(s->buf + s->len, RSTRING_PTR(bytes_v), add);
    s->len += add;
    return self;
}

static VALUE dom_stream_set_eof(VALUE self) {
    dom_stream_t *s;
    TypedData_Get_Struct(self, dom_stream_t, &dom_stream_data_type, s);
    s->eof = 1;
    return self;
}

static VALUE dom_stream_done(VALUE self) {
    dom_stream_t *s;
    TypedData_Get_Struct(self, dom_stream_t, &dom_stream_data_type, s);
    return (s->eof && s->consumed >= s->len) ? Qtrue : Qfalse;
}

/* ---- scanner helpers --------------------------------------------- */

static inline int dom_str_eq_ci(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower_c((unsigned char)a[i]) != ascii_lower_c((unsigned char)b[i])) return 0;
    }
    return 1;
}

static inline int dom_is_name_boundary(char c) {
    return c == '>' || c == '/' || c == ' ' || c == '\t' ||
           c == '\n' || c == '\r' || c == '\f';
}

/* Find offset past '>' for the tag opener starting at i. Handles
 * quoted attribute values. Returns 0 + sets *incomplete on truncation. */
static size_t scan_past_tag_close(const char *buf, size_t len, size_t i, int *incomplete) {
    /* assume buf[i] == '<' */
    i++;
    while (i < len) {
        char c = buf[i];
        if (c == '"' || c == '\'') {
            char q = c; i++;
            while (i < len && buf[i] != q) i++;
            if (i == len) { *incomplete = 1; return 0; }
            i++;
        } else if (c == '>') {
            return i + 1;
        } else {
            i++;
        }
    }
    *incomplete = 1;
    return 0;
}

/* Find offset past '-->' starting at the '<!--' position. */
static size_t scan_past_comment(const char *buf, size_t len, size_t i, int *incomplete) {
    /* assume buf[i..i+3] == '<!--' */
    i += 4;
    while (i + 2 < len) {
        if (buf[i] == '-' && buf[i+1] == '-' && buf[i+2] == '>') return i + 3;
        i++;
    }
    *incomplete = 1;
    return 0;
}

/* Find offset past ']]>' starting at the '<![CDATA[' position. */
static size_t scan_past_cdata(const char *buf, size_t len, size_t i, int *incomplete) {
    /* assume buf[i..i+2] == '<![' */
    i += 3;
    while (i + 2 < len) {
        if (buf[i] == ']' && buf[i+1] == ']' && buf[i+2] == '>') return i + 3;
        i++;
    }
    *incomplete = 1;
    return 0;
}

/* For '<script' or '<style': find offset past matching '</tag>'. */
static size_t scan_past_raw_text(const char *buf, size_t len, size_t i,
                                 const char *rt, size_t rl, int *incomplete) {
    size_t after_open = scan_past_tag_close(buf, len, i, incomplete);
    if (after_open == 0) return 0;
    i = after_open;
    while (i + 2 + rl < len) {
        if (buf[i] == '<' && buf[i+1] == '/' &&
            dom_str_eq_ci(buf + i + 2, rt, rl) &&
            dom_is_name_boundary(buf[i + 2 + rl])) {
            return scan_past_tag_close(buf, len, i, incomplete);
        }
        i++;
    }
    *incomplete = 1;
    return 0;
}

/* Identify the construct at buf[i] (which must be '<'). Outputs:
 *   *end             = offset past the construct (past '>' / past '</...>').
 *   *kind            = 0 unknown, 1 open-of-target, 2 close-of-target,
 *                      3 other-open, 4 skip-section (comment/cdata/script/style),
 *                      5 self-closing-of-target.
 *   *has_class_match = 1 if cls is set and the open tag carries it; only
 *                      meaningful when kind is 1 or 5.
 *
 * Returns 1 on success, 0 if incomplete (need more data) — caller checks
 * the *incomplete flag.
 */
static int dom_stream_classify(const char *buf, size_t len, size_t i,
                               const char *tag, size_t tag_len,
                               const char *cls, size_t cls_len,
                               size_t *end, int *kind, int *has_class_match,
                               int *incomplete) {
    *kind = 0; *has_class_match = 0;
    if (i + 1 >= len) { *incomplete = 1; return 0; }

    /* '<!' constructs */
    if (buf[i+1] == '!') {
        if (i + 4 <= len && buf[i+2] == '-' && buf[i+3] == '-') {
            size_t e = scan_past_comment(buf, len, i, incomplete);
            if (e == 0) return 0;
            *end = e; *kind = 4; return 1;
        }
        if (i + 3 <= len && buf[i+2] == '[') {
            size_t e = scan_past_cdata(buf, len, i, incomplete);
            if (e == 0) return 0;
            *end = e; *kind = 4; return 1;
        }
        /* doctype / unknown declaration — scan to '>' */
        size_t e = scan_past_tag_close(buf, len, i, incomplete);
        if (e == 0) return 0;
        *end = e; *kind = 4; return 1;
    }

    /* '</TAG>' */
    if (buf[i+1] == '/') {
        if (i + 2 + tag_len > len) { *incomplete = 1; return 0; }
        size_t e = scan_past_tag_close(buf, len, i, incomplete);
        if (e == 0) return 0;
        *end = e;
        if (dom_str_eq_ci(buf + i + 2, tag, tag_len) &&
            dom_is_name_boundary(buf[i + 2 + tag_len])) {
            *kind = 2;
        }
        return 1;
    }

    /* script/style — skip wholesale */
    {
        static const char *const RAW[]  = {"script", "style", NULL};
        static const size_t      RAWL[] = {6,        5,       0};
        for (int k = 0; RAW[k]; k++) {
            size_t rl = RAWL[k];
            if (i + 1 + rl <= len &&
                dom_str_eq_ci(buf + i + 1, RAW[k], rl) &&
                (i + 1 + rl == len || dom_is_name_boundary(buf[i + 1 + rl]))) {
                if (i + 1 + rl == len) { *incomplete = 1; return 0; }
                size_t e = scan_past_raw_text(buf, len, i, RAW[k], rl, incomplete);
                if (e == 0) return 0;
                *end = e; *kind = 4; return 1;
            }
        }
    }

    /* Check if it's our target tag. */
    int is_target =
        (i + 1 + tag_len <= len &&
         dom_str_eq_ci(buf + i + 1, tag, tag_len) &&
         (i + 1 + tag_len < len) &&
         dom_is_name_boundary(buf[i + 1 + tag_len]));
    if (i + 1 + tag_len >= len) { *incomplete = 1; return 0; }

    /* Walk attributes once, recording the class value if present.
     * Faster than two passes since most openings have <= 3 attrs. */
    size_t j = i + 1;
    while (j < len && !dom_is_name_boundary(buf[j])) j++;  /* skip tag name */
    if (j == len) { *incomplete = 1; return 0; }

    int found_class = 0;
    int self_closing = 0;
    while (j < len) {
        while (j < len && (buf[j] == ' ' || buf[j] == '\t' ||
                           buf[j] == '\n' || buf[j] == '\r')) j++;
        if (j == len) { *incomplete = 1; return 0; }
        if (buf[j] == '>') { j++; break; }
        if (buf[j] == '/' && j + 1 < len && buf[j+1] == '>') {
            self_closing = 1; j += 2; break;
        }
        size_t an_s = j;
        while (j < len && buf[j] != '=' && buf[j] != ' ' && buf[j] != '\t' &&
               buf[j] != '\n' && buf[j] != '\r' && buf[j] != '/' && buf[j] != '>') j++;
        if (j == len) { *incomplete = 1; return 0; }
        size_t an_e = j;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t' ||
                           buf[j] == '\n' || buf[j] == '\r')) j++;
        if (j == len) { *incomplete = 1; return 0; }
        size_t av_s = 0, av_e = 0;
        if (buf[j] == '=') {
            j++;
            while (j < len && (buf[j] == ' ' || buf[j] == '\t' ||
                               buf[j] == '\n' || buf[j] == '\r')) j++;
            if (j == len) { *incomplete = 1; return 0; }
            if (buf[j] == '"' || buf[j] == '\'') {
                char q = buf[j++];
                av_s = j;
                while (j < len && buf[j] != q) j++;
                if (j == len) { *incomplete = 1; return 0; }
                av_e = j; j++;
            } else {
                av_s = j;
                while (j < len && buf[j] != ' ' && buf[j] != '\t' &&
                       buf[j] != '\n' && buf[j] != '\r' && buf[j] != '>') j++;
                if (j == len) { *incomplete = 1; return 0; }
                av_e = j;
            }
        }
        if (is_target && cls != NULL && cls_len > 0 &&
            (an_e - an_s) == 5 && dom_str_eq_ci(buf + an_s, "class", 5)) {
            size_t v = av_s;
            while (v < av_e) {
                while (v < av_e && (buf[v] == ' ' || buf[v] == '\t')) v++;
                size_t t_s = v;
                while (v < av_e && buf[v] != ' ' && buf[v] != '\t') v++;
                if ((v - t_s) == cls_len && dom_str_eq_ci(buf + t_s, cls, cls_len)) {
                    found_class = 1; break;
                }
            }
        }
    }
    *end = j;

    if (is_target && (cls == NULL || found_class)) {
        *kind = self_closing ? 5 : 1;
        *has_class_match = 1;
    } else {
        *kind = 3;  /* other or non-matching-class target */
    }
    return 1;
}

/* Scan forward from consumed, looking for one complete row. Returns:
 *    1  -> found; row_start/row_end set.
 *    0  -> no row in current buffer (caller waits for feed/eof).
 *   -1  -> not enough data to decide (need more feed).
 */
static int dom_stream_scan_one(dom_stream_t *s, size_t *row_start, size_t *row_end) {
    size_t i = s->consumed;
    while (i < s->len) {
        if (s->buf[i] != '<') { i++; continue; }
        int inc = 0, kind = 0, ccm = 0;
        size_t end = 0;
        if (!dom_stream_classify(s->buf, s->len, i,
                                 s->tag, s->tag_len, s->cls, s->cls_len,
                                 &end, &kind, &ccm, &inc)) {
            if (inc) return -1;
            i++; continue;
        }
        if (kind == 5) {
            /* self-closing target — emit a row of just the open tag */
            *row_start = i;
            *row_end = end;
            return 1;
        }
        if (kind == 1) {
            /* Real opener — scan for matching close, depth tracked. */
            size_t opener = i;
            size_t j = end;
            int depth = 1;
            while (depth > 0 && j < s->len) {
                if (s->buf[j] != '<') { j++; continue; }
                int inc2 = 0, kind2 = 0, ccm2 = 0;
                size_t end2 = 0;
                if (!dom_stream_classify(s->buf, s->len, j,
                                         s->tag, s->tag_len, s->cls, s->cls_len,
                                         &end2, &kind2, &ccm2, &inc2)) {
                    if (inc2) return -1;
                    j++; continue;
                }
                /* For depth tracking we count ANY open/close of our tag
                 * (regardless of class) so nested same-tag siblings
                 * balance correctly. */
                if (kind2 == 2) depth--;
                else if (kind2 == 3 && j + 1 + s->tag_len < s->len &&
                         dom_str_eq_ci(s->buf + j + 1, s->tag, s->tag_len) &&
                         dom_is_name_boundary(s->buf[j + 1 + s->tag_len])) {
                    depth++;
                } else if (kind2 == 1) depth++;
                /* kind 4 (skip-section) and 5 (self-closing) and 3 (other tag)
                 * don't affect depth. */
                j = end2;
            }
            if (depth > 0) return -1;
            *row_start = opener;
            *row_end = j;
            return 1;
        }
        /* skip past any other construct */
        i = end;
    }
    return 0;
}

static VALUE dom_stream_next_row(VALUE self) {
    dom_stream_t *s;
    TypedData_Get_Struct(self, dom_stream_t, &dom_stream_data_type, s);
    size_t rs = 0, re = 0;
    int r = dom_stream_scan_one(s, &rs, &re);
    if (r != 1) return Qnil;
    VALUE row = rb_str_new(s->buf + rs, (long)(re - rs));
    rb_enc_associate(row, enc_utf8);
    s->consumed = re;
    /* Compact the buffer once consumed bytes dominate, freeing
     * memory back to the next feed. */
    if (s->consumed > 4096 && s->consumed * 2 > s->len) {
        size_t remaining = s->len - s->consumed;
        if (remaining > 0) memmove(s->buf, s->buf + s->consumed, remaining);
        s->len = remaining;
        s->consumed = 0;
    }
    return row;
}

/* ---- module init ------------------------------------------------- */

void Init_scrapetor_dom(VALUE mod_native) {
    dom_tag_table_init();
    VALUE doc_klass = rb_define_class_under(mod_native, "Document", rb_cObject);
    rb_define_alloc_func(doc_klass, NULL);  /* parse() is the only constructor */
    rb_define_singleton_method(doc_klass, "parse", dom_parse_html, 1);
    rb_define_singleton_method(doc_klass, "parallel_parse", dom_parallel_parse, 2);
    rb_define_singleton_method(doc_klass, "load_from_file", dom_native_load_from_file, 1);
    rb_define_method(doc_klass, "serialize_to_file", dom_native_serialize_to_file, 1);

    rb_define_method(doc_klass, "size",                dom_size,              0);
    rb_define_method(doc_klass, "html",                dom_html,              0);
    rb_define_method(doc_klass, "root_id",             dom_root_id,           0);
    rb_define_method(doc_klass, "node_type",           dom_node_type,         1);
    rb_define_method(doc_klass, "node_name",           dom_node_name,         1);
    rb_define_method(doc_klass, "node_attr",           dom_node_attr,         2);
    rb_define_method(doc_klass, "node_attributes",     dom_node_attributes,   1);
    rb_define_method(doc_klass, "node_text",           dom_node_text,         1);
    rb_define_method(doc_klass, "node_parent",         dom_node_parent,       1);
    rb_define_method(doc_klass, "node_first_child",    dom_node_first_child,  1);
    rb_define_method(doc_klass, "node_next_sibling",   dom_node_next_sibling, 1);
    rb_define_method(doc_klass, "node_prev_sibling",   dom_node_prev_sibling, 1);
    rb_define_method(doc_klass, "node_children",       dom_node_children,     1);
    rb_define_method(doc_klass, "node_element_children", dom_node_element_children, 1);
    rb_define_method(doc_klass, "node_is_element",     dom_node_is_element,   1);
    rb_define_method(doc_klass, "node_classes",        dom_node_classes,      1);
    rb_define_method(doc_klass, "run_chain",           dom_run_chain,         2);
    rb_define_method(doc_klass, "first_match",         dom_first_match,       2);
    rb_define_method(doc_klass, "extract_each_native", dom_extract_each,      6);
    rb_define_method(doc_klass, "extract_one_native",  dom_extract_one,       5);
    rb_define_method(doc_klass, "extract_one_h",       dom_extract_one_h,     3);
    rb_define_method(doc_klass, "extract_each_h",      dom_extract_each_h,    4);
    rb_define_method(doc_klass, "fast_at_css",         dom_fast_at_css,       3);
    rb_define_method(doc_klass, "fast_css",            dom_fast_css,          3);
    rb_define_method(doc_klass, "cache_get",           dom_cache_get,         2);
    rb_define_method(doc_klass, "cache_put",           dom_cache_put,         3);
    rb_define_method(doc_klass, "multi_css",           dom_multi_css,         3);

    rb_define_singleton_method(mod_native, "_register_element_methods",
                               register_element_native_methods, 1);
    rb_define_singleton_method(mod_native, "_register_node_methods",
                               register_node_native_methods, 1);
    rb_define_method(doc_klass, "batch_chain",         dom_batch_chain,       2);
    rb_define_method(doc_klass, "bulk_text",           dom_bulk_text,         1);
    rb_define_method(doc_klass, "bulk_attr",           dom_bulk_attr,         2);
    rb_define_method(doc_klass, "node_remove",         dom_node_remove,       1);
    rb_define_method(doc_klass, "node_set_inner_html", dom_node_set_inner_html, 2);

    rb_define_method(doc_klass, "_class_index_size", dom_class_index_size, 0);
    rb_define_method(doc_klass, "_class_index_keys", dom_class_index_keys, 0);

    /* Streaming row scanner. Constructed with an outer tag (required)
     * and an optional class filter; fed bytes from a Ruby IO, returns
     * one complete row's HTML at a time. */
    VALUE stream_klass = rb_define_class_under(mod_native, "Stream", rb_cObject);
    rb_define_alloc_func(stream_klass, dom_stream_alloc);
    rb_define_method(stream_klass, "initialize", dom_stream_initialize, -1);
    rb_define_method(stream_klass, "feed",      dom_stream_feed,        1);
    rb_define_method(stream_klass, "set_eof",   dom_stream_set_eof,     0);
    rb_define_method(stream_klass, "done?",     dom_stream_done,        0);
    rb_define_method(stream_klass, "next_row",  dom_stream_next_row,    0);
}
