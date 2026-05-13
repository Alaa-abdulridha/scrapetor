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

#define DOM_NIL 0xFFFFFFFFu   /* sentinel for absent index */

#define DOM_INIT_NODES 64
#define DOM_INIT_ATTRS 128

typedef struct {
    uint32_t name_off;
    uint32_t name_len;
    uint32_t val_off;
    uint32_t val_len;
} dom_attr_t;

typedef struct {
    uint8_t  type;
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
} dom_node_t;

/* Open-addressing hashmap: string-key (offset into html_buf) -> Vec<u32> */

typedef struct {
    uint32_t key_off;
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

struct dom_doc {
    /* Zero-copy input. We hold a reference to a frozen Ruby String and
     * point html_buf at its bytes directly. The mark callback on the
     * typed_data wrapper pins the String so the bytes stay live. */
    VALUE    html_str_value;
    const char *html_buf;
    size_t   html_len;

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
};

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

static dom_index_entry_t *dom_index_get_or_create(dom_index_t *ix, const char *key, size_t klen, uint32_t hash, const char *html_buf) {
    if (ix->count * 2 > ix->cap) dom_index_resize(ix, ix->cap * 2);
    size_t mask = ix->cap - 1;
    size_t k = hash & mask;
    while (ix->buckets[k].used) {
        dom_index_entry_t *e = &ix->buckets[k];
        if (e->key_hash == hash &&
            dom_streq_ci(html_buf + e->key_off, e->key_len, key, klen)) {
            return e;
        }
        k = (k + 1) & mask;
    }
    dom_index_entry_t *e = &ix->buckets[k];
    e->used = 1;
    e->key_off = (uint32_t)(key - html_buf);
    e->key_len = (uint32_t)klen;
    e->key_hash = hash;
    e->cap = 4;
    e->count = 0;
    e->ids = (uint32_t *)malloc(sizeof(uint32_t) * e->cap);
    ix->count++;
    return e;
}

static dom_index_entry_t *dom_index_lookup(dom_index_t *ix, const char *key, size_t klen, const char *html_buf) {
    uint32_t hash = fnv1a_ci(key, klen);
    size_t mask = ix->cap - 1;
    size_t k = hash & mask;
    while (ix->buckets[k].used) {
        dom_index_entry_t *e = &ix->buckets[k];
        if (e->key_hash == hash &&
            dom_streq_ci(html_buf + e->key_off, e->key_len, key, klen)) {
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
                dom_index_get_or_create(&d->class_idx, val + s, i - s, hash, d->html_buf);
            dom_index_push(e, node_id);
        }
    }
}

static void index_id(dom_doc_t *d, const char *val, size_t vlen, uint32_t node_id) {
    if (vlen == 0) return;
    uint32_t hash = fnv1a_ci(val, vlen);
    dom_index_entry_t *e = dom_index_get_or_create(&d->id_idx, val, vlen, hash, d->html_buf);
    dom_index_push(e, node_id);
}

static void index_tag(dom_doc_t *d, const char *name, size_t nlen, uint32_t node_id) {
    if (nlen == 0) return;
    uint32_t hash = fnv1a_ci(name, nlen);
    dom_index_entry_t *e = dom_index_get_or_create(&d->tag_idx, name, nlen, hash, d->html_buf);
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
            while (pos < len && is_name_byte((unsigned char)html[pos])) pos++;
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
            while (pos < len && is_name_byte((unsigned char)html[pos])) pos++;
            size_t nlen = pos - ns;
            if (nlen == 0) continue;
            const char *tag_p = html + ns;

            uint32_t n_attrs = 0;
            const char *cls_p = NULL; size_t cls_len = 0;
            const char *id_p  = NULL; size_t id_len  = 0;
            int self_closing = 0;

            /* attributes */
            while (pos < len) {
                while (pos < len && is_ws_byte((unsigned char)html[pos])) pos++;
                if (pos >= len) break;
                char ch = html[pos];
                if (ch == '>') { pos++; break; }
                if (ch == '/' && pos + 1 < len && html[pos+1] == '>') {
                    self_closing = 1; pos += 2; break;
                }
                /* attr name */
                size_t an_s = pos;
                while (pos < len) {
                    unsigned char nc = (unsigned char)html[pos];
                    if (nc == '=' || nc == '>' || nc == '/' || is_ws_byte(nc)) break;
                    pos++;
                }
                size_t an_len = pos - an_s;
                size_t av_s = 0, av_len = 0;
                while (pos < len && is_ws_byte((unsigned char)html[pos])) pos++;
                if (pos < len && html[pos] == '=') {
                    pos++;
                    while (pos < len && is_ws_byte((unsigned char)html[pos])) pos++;
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

/* Append the decoded textual content of a subtree to `buf`. Decodes the
 * minimal entity set (the broader Scrapetor::Entities table lives in
 * Ruby). */
static void append_subtree_text(dom_doc_t *d, uint32_t nid, VALUE buf) {
    dom_node_t *n = &d->nodes[nid];
    if (n->type == DOM_TYPE_TEXT) {
        /* Inline entity decode for the minimal set. */
        const char *p = d->html_buf + n->text_off;
        size_t L = n->text_len;
        size_t i = 0, start = 0;
        while (i < L) {
            if (p[i] == '&') {
                if (i > start) rb_str_buf_cat(buf, p + start, i - start);
                size_t j = i + 1;
                size_t cap = (L - j < 10) ? (L - j) : 10;
                while (j < i + 1 + cap && p[j] != ';' && p[j] != '&' && p[j] != ' ' && p[j] != '<') j++;
                int matched = 0;
                if (j < L && p[j] == ';') {
                    size_t elen = j - i - 1;
                    const char *e = p + i + 1;
                    char rep[1]; int rl = 0;
                    if      (elen == 3 && memcmp(e, "amp", 3) == 0)  { rep[0] = '&'; rl = 1; }
                    else if (elen == 2 && memcmp(e, "lt", 2) == 0)   { rep[0] = '<'; rl = 1; }
                    else if (elen == 2 && memcmp(e, "gt", 2) == 0)   { rep[0] = '>'; rl = 1; }
                    else if (elen == 4 && memcmp(e, "quot", 4) == 0) { rep[0] = '"'; rl = 1; }
                    else if (elen == 4 && memcmp(e, "apos", 4) == 0) { rep[0] = '\''; rl = 1; }
                    else if (elen == 4 && memcmp(e, "nbsp", 4) == 0) { rep[0] = ' '; rl = 1; }
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
    /* Skip the helper hash for class/id; those have dedicated cached
     * spans + indexes already. */
    if (nlen == 5 && strncasecmp(name, "class", 5) == 0) return NULL;
    if (nlen == 2 && strncasecmp(name, "id", 2) == 0)    return NULL;

    uint32_t hash = fnv1a_ci(name, nlen);
    /* Probe — if we've built this name's bucket already, return it. */
    {
        size_t mask = d->attr_idx.cap - 1;
        size_t k = hash & mask;
        while (d->attr_idx.buckets[k].used) {
            dom_index_entry_t *e = &d->attr_idx.buckets[k];
            if (e->key_hash == hash &&
                dom_streq_ci(d->html_buf + e->key_off, e->key_len, name, nlen)) {
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
                strncasecmp(d->html_buf + ax->name_off, name, nlen) == 0) {
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
            &d->attr_idx, d->html_buf, 0, fnv1a_ci("", 0), d->html_buf);
        empty->key_len = 0;
        return empty;
    }

found_name: {
        dom_index_entry_t *e = dom_index_get_or_create(
            &d->attr_idx, d->html_buf + name_off, nlen, hash, d->html_buf);
        for (uint32_t i = 0; i < d->n_nodes; i++) {
            dom_node_t *nd = &d->nodes[i];
            if (nd->type != DOM_TYPE_ELEMENT) continue;
            for (uint32_t k = 0; k < nd->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[nd->attr_first + k];
                if (ax->name_len == nlen &&
                    strncasecmp(d->html_buf + ax->name_off, name, nlen) == 0) {
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
    /* Pin the Ruby String backing html_buf so GC doesn't collect it
     * out from under us. */
    if (d && d->html_str_value != Qnil && d->html_str_value != 0) {
        rb_gc_mark(d->html_str_value);
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
     * SerpApi-style results rarely exceed 4-5 distinct child tag names. */
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

static void ensure_parsed(dom_doc_t *d) {
    if (d->parsed) return;
    d->parsed = 1;  /* set before parse so we don't re-enter on error */
    dom_parse(d);
    compute_dfs_out(d);
    compute_position_indices(d);
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
    d->html_len = (size_t)RSTRING_LEN(owned);

    /* Tokenisation deferred — ensure_parsed runs it on first query. */
    return TypedData_Wrap_Struct(klass, &dom_doc_data_type, d);
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
        p = d->html_buf + n->tag_off; l = n->tag_len;
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
            strncasecmp(d->html_buf + a->name_off, np, (size_t)nl) == 0) {
            VALUE v = rb_str_new(d->html_buf + a->val_off, (long)a->val_len);
            rb_enc_associate(v, enc_utf8);
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
        VALUE name = rb_str_new(d->html_buf + a->name_off, (long)a->name_len);
        VALUE val  = rb_str_new(d->html_buf + a->val_off,  (long)a->val_len);
        rb_enc_associate(name, enc_utf8);
        rb_enc_associate(val,  enc_utf8);
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

static VALUE dom_node_first_child(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = d->nodes[i].first_child;
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_next_sibling(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = d->nodes[i].next_sibling;
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_prev_sibling(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    uint32_t c = d->nodes[i].prev_sibling;
    return (c == DOM_NIL) ? Qnil : UINT2NUM(c);
}

static VALUE dom_node_children(VALUE self, VALUE id) {
    dom_doc_t *d = get_dom(self);
    uint32_t i = NUM2UINT(id);
    VALIDATE_ID(d, i);
    VALUE ary = rb_ary_new();
    uint32_t c = d->nodes[i].first_child;
    while (c != DOM_NIL) {
        rb_ary_push(ary, UINT2NUM(c));
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
        if (a->name_len == 5 && strncasecmp(d->html_buf + a->name_off, "class", 5) == 0) {
            const char *vp = d->html_buf + a->val_off;
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
        VALUE s = rb_str_new(d->html_buf + e->key_off, (long)e->key_len);
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

typedef struct {
    const char *name;
    size_t      len;
    int         op;     /* 0 exists, 1 eq, 2 prefix, 3 suffix, 4 contains, 5 word, 6 dash */
    const char *val;
    size_t      vlen;
} c_attr_m;

/* "Simple" atom — used as the inner selector for :not / :is / :has so
 * the structure is non-recursive. Carries the same matchable surface
 * as c_atom (tag/class/id/attrs/positional+boolean pseudos) but no
 * combinator and no recursive pseudos (NOT/IS/HAS bits are ignored
 * here — the inner of an inner is fallback territory). */
typedef struct {
    const char *tag;     size_t tag_len;
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
} c_simple_atom;

typedef struct {
    const char *tag;     size_t tag_len;
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

/* Populate a c_simple_atom from a Ruby `[tag, classes, id, attrs]`
 * array. Used both directly (for inner :not/:is/:has atoms) and
 * indirectly (build_atom copies the simple part the same way). */
static int build_simple_atom(VALUE sel_v, c_simple_atom *out) {
    memset(out, 0, sizeof(*out));
    if (!RB_TYPE_P(sel_v, T_ARRAY) || RARRAY_LEN(sel_v) < 4) return 0;

    VALUE tag = rb_ary_entry(sel_v, 0);
    if (!NIL_P(tag)) {
        if (!RB_TYPE_P(tag, T_STRING)) return 0;
        out->tag = RSTRING_PTR(tag); out->tag_len = (size_t)RSTRING_LEN(tag);
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
        if (!RB_TYPE_P(n, T_STRING)) return 0;
        out->attrs[i].name = RSTRING_PTR(n);
        out->attrs[i].len  = (size_t)RSTRING_LEN(n);
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

    /* Optional pseudo data on inner atoms. Same layout as the outer
     * atom but only the leaf pseudos (flags + nth coefficients) — the
     * Ruby compiler refuses to emit a c_simple_atom that contains a
     * recursive NOT/IS/HAS bit. */
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
        for (int k = 5; k <= 7; k++) {
            VALUE inner = rb_ary_entry(pseudo, k);
            if (RB_TYPE_P(inner, T_ARRAY)) total += RARRAY_LEN(inner);
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

    VALUE not_arr = rb_ary_entry(pseudo, 5);
    if (RB_TYPE_P(not_arr, T_ARRAY) && RARRAY_LEN(not_arr) > 0) {
        long m = RARRAY_LEN(not_arr);
        c_simple_atom *base = pool + *pool_used;
        for (long i = 0; i < m; i++) {
            if (!build_simple_atom(rb_ary_entry(not_arr, i), &base[i])) return 0;
        }
        out->not_inner = base;
        out->n_not_inner = (int)m;
        *pool_used += m;
    }

    VALUE is_arr = rb_ary_entry(pseudo, 6);
    if (RB_TYPE_P(is_arr, T_ARRAY) && RARRAY_LEN(is_arr) > 0) {
        long m = RARRAY_LEN(is_arr);
        c_simple_atom *base = pool + *pool_used;
        for (long i = 0; i < m; i++) {
            if (!build_simple_atom(rb_ary_entry(is_arr, i), &base[i])) return 0;
        }
        out->is_inner = base;
        out->n_is_inner = (int)m;
        *pool_used += m;
    }

    VALUE has_arr = rb_ary_entry(pseudo, 7);
    if (RB_TYPE_P(has_arr, T_ARRAY) && RARRAY_LEN(has_arr) > 0) {
        long m = RARRAY_LEN(has_arr);
        c_simple_atom *base = pool + *pool_used;
        for (long i = 0; i < m; i++) {
            if (!build_simple_atom(rb_ary_entry(has_arr, i), &base[i])) return 0;
        }
        out->has_inner = base;
        out->n_has_inner = (int)m;
        *pool_used += m;
    }

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
static inline int is_first_of_type(dom_doc_t *d, uint32_t id) {
    dom_node_t *n = &d->nodes[id];
    uint32_t s = n->prev_sibling;
    while (s != DOM_NIL) {
        dom_node_t *m = &d->nodes[s];
        if (m->type == DOM_TYPE_ELEMENT &&
            m->tag_len == n->tag_len &&
            strncasecmp(d->html_buf + m->tag_off,
                        d->html_buf + n->tag_off, n->tag_len) == 0) return 0;
        s = m->prev_sibling;
    }
    return 1;
}

static inline int is_last_of_type(dom_doc_t *d, uint32_t id) {
    dom_node_t *n = &d->nodes[id];
    uint32_t s = n->next_sibling;
    while (s != DOM_NIL) {
        dom_node_t *m = &d->nodes[s];
        if (m->type == DOM_TYPE_ELEMENT &&
            m->tag_len == n->tag_len &&
            strncasecmp(d->html_buf + m->tag_off,
                        d->html_buf + n->tag_off, n->tag_len) == 0) return 0;
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
            strncasecmp(d->html_buf + ax->name_off, name, nlen) == 0) {
            /* "false" value -> falsy, otherwise truthy. */
            if (ax->val_len == 5 &&
                strncasecmp(d->html_buf + ax->val_off, "false", 5) == 0) return 0;
            return 1;
        }
    }
    return 0;
}

/* Forward declarations: simple-atom matcher and atom matcher recurse
 * across :has() / :is() / :not() evaluation. */
static int matches_simple_atom(dom_doc_t *d, uint32_t id, const c_simple_atom *a);
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

/* Match the simple (non-pseudo) part of an atom against a node. Pulled
 * out so :not / :is / :has can reuse the same predicate without the
 * recursive pseudo machinery. */
static int matches_simple_atom(dom_doc_t *d, uint32_t id, const c_simple_atom *a) {
    dom_node_t *n = &d->nodes[id];
    if (__builtin_expect(n->type != DOM_TYPE_ELEMENT, 0)) return 0;
    if (a->tag) {
        if (n->tag_len != a->tag_len) return 0;
        if (strncasecmp(d->html_buf + n->tag_off, a->tag, a->tag_len) != 0) return 0;
    }
    if (a->n_classes > 0) {
        if (n->class_off == DOM_NIL) return 0;
        const char *cls_p = d->html_buf + n->class_off;
        size_t cls_len = n->class_len;
        for (int i = 0; i < a->n_classes; i++) {
            if (!class_in_attr(cls_p, cls_len, a->classes[i], a->class_lens[i])) return 0;
        }
    }
    if (a->id) {
        if (n->id_off == DOM_NIL) return 0;
        if (n->id_len != a->id_len) return 0;
        if (memcmp(d->html_buf + n->id_off, a->id, a->id_len) != 0) return 0;
    }
    for (int i = 0; i < a->n_attrs; i++) {
        int found = 0; size_t avl = 0; const char *avp = NULL;
        for (uint32_t k = 0; k < n->attr_count; k++) {
            dom_attr_t *ax = &d->attrs[n->attr_first + k];
            if (ax->name_len == a->attrs[i].len &&
                strncasecmp(d->html_buf + ax->name_off, a->attrs[i].name, a->attrs[i].len) == 0) {
                avp = d->html_buf + ax->val_off; avl = ax->val_len; found = 1; break;
            }
        }
        if (!found) return 0;
        const char *vp = a->attrs[i].val; size_t vl = a->attrs[i].vlen;
        switch (a->attrs[i].op) {
        case 0: break;
        case 1: if (avl != vl || memcmp(avp, vp, vl) != 0) return 0; break;
        case 2: if (avl < vl || memcmp(avp, vp, vl) != 0) return 0; break;
        case 3: if (avl < vl || memcmp(avp + avl - vl, vp, vl) != 0) return 0; break;
        case 4: {
            int hit = 0;
            if (avl >= vl) for (size_t k = 0; k + vl <= avl; k++) if (memcmp(avp + k, vp, vl) == 0) { hit = 1; break; }
            if (!hit) return 0;
            break;
        }
        case 5: if (!class_in_attr(avp, avl, vp, vl)) return 0; break;
        case 6: {
            if (avl < vl || memcmp(avp, vp, vl) != 0) return 0;
            if (avl > vl && avp[vl] != '-') return 0;
            break;
        }
        default: return 0;
        }
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
                (node->tag_len == 1 && (d->html_buf[node->tag_off] == 'a' || d->html_buf[node->tag_off] == 'A')) ||
                (node->tag_len == 4 && strncasecmp(d->html_buf + node->tag_off, "area", 4) == 0);
            if (!is_link_tag) return 0;
            int has_href = 0;
            for (uint32_t k = 0; k < node->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[node->attr_first + k];
                if (ax->name_len == 4 &&
                    strncasecmp(d->html_buf + ax->name_off, "href", 4) == 0) {
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
    return 1;
}

/* For an inner :has() selector, pick the narrowest structural index
 * available and use a binary search over its (already id-sorted) entry
 * list combined with the dfs_in / dfs_out range encoding to check
 * "does this subtree contain a match" in O(log K) instead of walking
 * the whole subtree. K = number of nodes carrying the chosen class/id/
 * tag globally; on a SerpApi-style page that's typically a handful.
 *
 * The index entry pointer is cached on the c_simple_atom so the hash
 * lookup runs once per query, not once per candidate. On `div:has(.x)`
 * over 100 divs that cuts ~5 μs of redundant hashing. */
static int has_descendant_via_index(dom_doc_t *d, uint32_t parent_id,
                                    const c_simple_atom *a) {
    dom_index_entry_t *e = (dom_index_entry_t *)a->cached_index;
    if (e == NULL) {
        if (a->id) {
            e = dom_index_lookup(&d->id_idx, a->id, a->id_len, d->html_buf);
            if (!e) { ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)2; return 0; }
        } else if (a->n_classes > 0) {
            for (int i = 0; i < a->n_classes; i++) {
                dom_index_entry_t *ec = dom_index_lookup(
                    &d->class_idx, a->classes[i], a->class_lens[i], d->html_buf);
                if (!ec) { ((c_simple_atom *)a)->cached_index = (void *)(uintptr_t)2; return 0; }
                if (!e || ec->count < e->count) e = ec;
            }
        } else if (a->tag) {
            e = dom_index_lookup(&d->tag_idx, a->tag, a->tag_len, d->html_buf);
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

static int element_matches_atom(dom_doc_t *d, uint32_t id, const c_atom *a) {
    dom_node_t *n = &d->nodes[id];
    if (__builtin_expect(n->type != DOM_TYPE_ELEMENT, 0)) return 0;
    if (a->tag) {
        if (n->tag_len != a->tag_len) return 0;
        if (strncasecmp(d->html_buf + n->tag_off, a->tag, a->tag_len) != 0) return 0;
    }
    if (a->n_classes > 0 || a->id || a->n_attrs > 0) {
        /* Class/id checks read from the cached spans on the node —
         * populated at parse time so this is O(1) instead of scanning
         * the attribute array on every match. */
        if (a->n_classes > 0) {
            if (n->class_off == DOM_NIL) return 0;
            const char *cls_p = d->html_buf + n->class_off;
            size_t cls_len = n->class_len;
            for (int i = 0; i < a->n_classes; i++) {
                if (!class_in_attr(cls_p, cls_len, a->classes[i], a->class_lens[i])) return 0;
            }
        }
        if (a->id) {
            if (n->id_off == DOM_NIL) return 0;
            if (n->id_len != a->id_len) return 0;
            if (memcmp(d->html_buf + n->id_off, a->id, a->id_len) != 0) return 0;
        }
        for (int i = 0; i < a->n_attrs; i++) {
            int found = 0; size_t avl = 0; const char *avp = NULL;
            for (uint32_t k = 0; k < n->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[n->attr_first + k];
                if (ax->name_len == a->attrs[i].len &&
                    strncasecmp(d->html_buf + ax->name_off, a->attrs[i].name, a->attrs[i].len) == 0) {
                    avp = d->html_buf + ax->val_off; avl = ax->val_len; found = 1; break;
                }
            }
            if (!found) return 0;
            const char *vp = a->attrs[i].val; size_t vl = a->attrs[i].vlen;
            switch (a->attrs[i].op) {
            case 0: break;
            case 1: if (avl != vl || memcmp(avp, vp, vl) != 0) return 0; break;
            case 2: if (avl < vl || memcmp(avp, vp, vl) != 0) return 0; break;
            case 3: if (avl < vl || memcmp(avp + avl - vl, vp, vl) != 0) return 0; break;
            case 4: {
                int hit = 0;
                if (avl >= vl) for (size_t k = 0; k + vl <= avl; k++) if (memcmp(avp + k, vp, vl) == 0) { hit = 1; break; }
                if (!hit) return 0;
                break;
            }
            case 5: if (!class_in_attr(avp, avl, vp, vl)) return 0; break;
            case 6: {
                if (avl < vl || memcmp(avp, vp, vl) != 0) return 0;
                if (avl > vl && avp[vl] != '-') return 0;
                break;
            }
            default: return 0;
            }
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
                (n->tag_len == 1 && (d->html_buf[n->tag_off] == 'a' || d->html_buf[n->tag_off] == 'A')) ||
                (n->tag_len == 4 && strncasecmp(d->html_buf + n->tag_off, "area", 4) == 0);
            if (!is_link_tag) return 0;
            int has_href = 0;
            for (uint32_t k = 0; k < n->attr_count; k++) {
                dom_attr_t *ax = &d->attrs[n->attr_first + k];
                if (ax->name_len == 4 &&
                    strncasecmp(d->html_buf + ax->name_off, "href", 4) == 0) {
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
        /* in-scope */
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
 * of node ids. */
static VALUE dom_run_chain(VALUE self, VALUE plan_v, VALUE scope_v) {
    dom_doc_t *d = get_dom(self);
    if (!RB_TYPE_P(plan_v, T_ARRAY)) rb_raise(rb_eArgError, "plan must be Array");
    long n = RARRAY_LEN(plan_v);
    if (n == 0) return rb_ary_new();

    uint32_t scope_id = NIL_P(scope_v) ? DOM_NIL : NUM2UINT(scope_v);

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
            if (RSTRING_LEN(combo) == 10 && memcmp(RSTRING_PTR(combo), "descendant", 10) == 0) atoms[i].combinator = 1;
            else if (RSTRING_LEN(combo) == 5 && memcmp(RSTRING_PTR(combo), "child", 5) == 0)   atoms[i].combinator = 2;
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
        dom_index_entry_t *e = dom_index_lookup(&d->id_idx, last->id, last->id_len, d->html_buf);
        if (e) { cands = e->ids; n_cands = e->count; }
    } else if (last->n_classes > 0) {
        /* pick smallest class set */
        dom_index_entry_t *best = NULL;
        for (int i = 0; i < last->n_classes; i++) {
            dom_index_entry_t *e = dom_index_lookup(&d->class_idx, last->classes[i], last->class_lens[i], d->html_buf);
            if (!e) { best = NULL; break; }
            if (!best || e->count < best->count) best = e;
        }
        if (best) { cands = best->ids; n_cands = best->count; }
    } else if (last->tag) {
        dom_index_entry_t *e = dom_index_lookup(&d->tag_idx, last->tag, last->tag_len, d->html_buf);
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
     * disabled / removed / hidden / etc." — SerpApi parsers use it
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
        for (size_t i = 0; i < n_cands; i++) {
            values[n_values++] = UINT2NUM(cands[i]);
        }
    } else if (set_diff_bypass) {
        dom_index_entry_t *e_not =
            dom_index_lookup(&d->class_idx,
                             last->not_inner[0].classes[0],
                             last->not_inner[0].class_lens[0],
                             d->html_buf);
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
            for (size_t i = 0; i < n_cands; i++) values[n_values++] = UINT2NUM(cands[i]);
        } else {
            /* Merge walk: both lists sorted by id (insertion order at parse). */
            size_t dis_pos = 0;
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                while (dis_pos < e_not->count && e_not->ids[dis_pos] < id) dis_pos++;
                if (dis_pos < e_not->count && e_not->ids[dis_pos] == id) continue;
                EMIT_ID(id);
            }
        }
    } else if (tag_class_bypass) {
        for (size_t i = 0; i < n_cands; i++) {
            dom_node_t *cn = &d->nodes[cands[i]];
            if (cn->tag_len != last->tag_len) continue;
            if (strncasecmp(d->html_buf + cn->tag_off, last->tag, last->tag_len) != 0) continue;
            EMIT_ID(cands[i]);
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
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                uint32_t p = d->nodes[id].parent;
                if (p == DOM_NIL) continue;
                if (!element_matches_atom(d, p, left)) continue;
                EMIT_ID(id);
            }
        } else if (n == 2 && atoms[1].combinator == 1 /* descendant */) {
            /* Specialised n=2 descendant path: A B. */
            c_atom *left = &atoms[0];
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                uint32_t cur = d->nodes[id].parent;
                int matched = 0;
                while (cur != DOM_NIL && d->nodes[cur].type == DOM_TYPE_ELEMENT) {
                    if (element_matches_atom(d, cur, left)) { matched = 1; break; }
                    cur = d->nodes[cur].parent;
                }
                if (matched) EMIT_ID(id);
            }
        } else {
            for (size_t i = 0; i < n_cands; i++) {
                uint32_t id = cands[i];
                if (!last_pre_matched && !element_matches_atom(d, id, last)) continue;
                if (!match_chain_backward(d, id, atoms, (int)n, (int)n - 2, DOM_NIL)) continue;
                EMIT_ID(id);
            }
        }
    } else if (!prefilter_bypass) {
        for (size_t i = 0; i < n_cands; i++) {
            uint32_t id = cands[i];
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

#undef EMIT_ID

    /* One allocation + memcpy instead of N pushes. */
    VALUE result = (n_values == 0) ? rb_ary_new() : rb_ary_new_from_values((long)n_values, values);
    if (values_on_heap) free(values);
    if (cands_owned) free(cands);
    return result;
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

/* Bulk text/attr extractors. Same machinery as dom_node_text /
 * dom_node_attr, but they take an Array<id> and return Array<String>
 * in one Ruby/C round trip — used by the css() boundary for
 * `selector::text` and `selector::attr(name)` queries so a 100-item
 * result set costs 1 boundary crossing instead of 100. */
static VALUE dom_bulk_text(VALUE self, VALUE ids_v) {
    dom_doc_t *d = get_dom(self);
    Check_Type(ids_v, T_ARRAY);
    long n = RARRAY_LEN(ids_v);
    VALUE out = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE id_v = rb_ary_entry(ids_v, i);
        uint32_t id = NUM2UINT(id_v);
        if (id >= d->n_nodes) { rb_ary_push(out, rb_utf8_str_new("", 0)); continue; }
        VALUE buf = rb_utf8_str_new("", 0);
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
                strncasecmp(d->html_buf + ax->name_off, nm, nm_len) == 0) {
                got = rb_utf8_str_new(d->html_buf + ax->val_off, (long)ax->val_len);
                break;
            }
        }
        rb_ary_push(out, got);
    }
    return out;
}

/* ---- module init ------------------------------------------------- */

void Init_scrapetor_dom(VALUE mod_native) {
    VALUE doc_klass = rb_define_class_under(mod_native, "Document", rb_cObject);
    rb_define_alloc_func(doc_klass, NULL);  /* parse() is the only constructor */
    rb_define_singleton_method(doc_klass, "parse", dom_parse_html, 1);

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
    rb_define_method(doc_klass, "batch_chain",         dom_batch_chain,       2);
    rb_define_method(doc_klass, "bulk_text",           dom_bulk_text,         1);
    rb_define_method(doc_klass, "bulk_attr",           dom_bulk_attr,         2);

    rb_define_method(doc_klass, "_class_index_size", dom_class_index_size, 0);
    rb_define_method(doc_klass, "_class_index_keys", dom_class_index_keys, 0);
}
