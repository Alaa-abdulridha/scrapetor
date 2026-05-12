/*
 * scrapetor_native.c
 *
 * Streaming HTML extraction engine.
 *
 * Tokenizes HTML in a single forward pass without constructing a DOM,
 * matches a schema of "repeated" blocks and field selectors during
 * tokenization, and emits structured records as repeated-block close
 * tags fire. One Ruby boundary crossing per document.
 *
 * Compatible with Ruby 2.0+ (uses only the stable public C API:
 * rb_define_module, rb_str_new, rb_hash_aset, rb_ary_push, RSTRING_*,
 * ID2SYM, NIL_P, RTEST, DBL2NUM, rb_enc_associate).
 *
 * Workload target: §16 wedge from plan.md — extract N product cards
 * (title, price-as-money, absolute URL, image URL) from an e-commerce
 * listing page. Designed to beat Nokolexbor on this workload by
 * eliminating DOM construction and per-field C<->Ruby crossings.
 *
 * Selector subset supported in the native fast path:
 *   tag                  e.g.  div
 *   .class               e.g.  .product-card
 *   tag.class            e.g.  span.price
 *
 * Schemas that exceed this subset transparently fall back to Ruby.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

rb_encoding *enc_utf8;
static inline VALUE u8(VALUE s) { rb_enc_associate(s, enc_utf8); return s; }

#define MAX_STACK            1024
#define MAX_FIELDS             64
#define MAX_GROUPS             16
#define MAX_ATTRS              64
#define MAX_CLASSES_PER_SEL     8
#define MAX_ATTRMATCH_PER_SEL   8
#define FIELD_BUF_INIT         64

#define FT_TEXT     0
#define FT_MONEY    1
#define FT_URL      2
#define FT_INTEGER  3
#define FT_FLOAT    4

#define AOP_EXISTS    0
#define AOP_EQ        1
#define AOP_PREFIX    2
#define AOP_SUFFIX    3
#define AOP_CONTAINS  4
#define AOP_WORD      5
#define AOP_DASH      6

#define COMB_NONE        0
#define COMB_DESCENDANT  1
#define COMB_CHILD       2

#ifndef RB_TYPE_P
#  define RB_TYPE_P(v, t) (TYPE(v) == (t))
#endif

/* ---- string-view + helpers ----------------------------------------- */

typedef struct { const char *p; size_t len; } strv;

static inline int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline int strv_ieq_cstr(strv a, const char *s, size_t n) {
    if (a.len != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)a.p[i]) != ascii_lower((unsigned char)s[i])) return 0;
    }
    return 1;
}

static inline int strv_ieq(strv a, strv b) {
    if (a.len != b.len) return 0;
    for (size_t i = 0; i < a.len; i++) {
        if (ascii_lower((unsigned char)a.p[i]) != ascii_lower((unsigned char)b.p[i])) return 0;
    }
    return 1;
}

static int class_present(strv classes, strv cls) {
    if (cls.len == 0) return 1;
    size_t i = 0;
    while (i < classes.len) {
        while (i < classes.len && (classes.p[i] == ' ' || classes.p[i] == '\t' || classes.p[i] == '\n' || classes.p[i] == '\r' || classes.p[i] == '\f')) i++;
        size_t j = i;
        while (j < classes.len && classes.p[j] != ' ' && classes.p[j] != '\t' && classes.p[j] != '\n' && classes.p[j] != '\r' && classes.p[j] != '\f') j++;
        if (j - i == cls.len && memcmp(classes.p + i, cls.p, cls.len) == 0) return 1;
        i = j;
    }
    return 0;
}

typedef struct {
    strv name;
    int  op;
    strv val;
} attr_match;

typedef struct {
    strv       tag;
    strv       classes[MAX_CLASSES_PER_SEL];
    int        n_classes;
    strv       id;
    attr_match attrs[MAX_ATTRMATCH_PER_SEL];
    int        n_attrs;
} ssel;

static inline int strv_eq_bytes(strv a, strv b) {
    return a.len == b.len && memcmp(a.p, b.p, a.len) == 0;
}

static int strv_contains(strv hay, strv needle) {
    if (needle.len == 0) return 0;
    if (hay.len < needle.len) return 0;
    for (size_t i = 0; i <= hay.len - needle.len; i++) {
        if (memcmp(hay.p + i, needle.p, needle.len) == 0) return 1;
    }
    return 0;
}

static int dash_match(strv hay, strv needle) {
    if (needle.len == 0) return hay.len == 0;
    if (hay.len < needle.len) return 0;
    if (memcmp(hay.p, needle.p, needle.len) != 0) return 0;
    if (hay.len == needle.len) return 1;
    return hay.p[needle.len] == '-';
}

static int sel_match(const ssel *s, strv tag, strv cls_attr, strv id_attr,
                     const strv attrs[][2], int n_attrs) {
    if (s->tag.len > 0 && !strv_ieq(s->tag, tag)) return 0;
    for (int i = 0; i < s->n_classes; i++) {
        if (!class_present(cls_attr, s->classes[i])) return 0;
    }
    if (s->id.len > 0) {
        if (id_attr.len == 0) return 0;
        if (id_attr.len != s->id.len || memcmp(id_attr.p, s->id.p, s->id.len) != 0) return 0;
    }
    for (int i = 0; i < s->n_attrs; i++) {
        strv av = { NULL, 0 };
        int found = 0;
        for (int a = 0; a < n_attrs; a++) {
            if (strv_ieq(attrs[a][0], s->attrs[i].name)) {
                av = attrs[a][1];
                found = 1;
                break;
            }
        }
        if (!found) return 0;
        switch (s->attrs[i].op) {
            case AOP_EXISTS:
                /* presence sufficient */
                break;
            case AOP_EQ:
                if (!strv_eq_bytes(av, s->attrs[i].val)) return 0;
                break;
            case AOP_PREFIX:
                if (av.len < s->attrs[i].val.len ||
                    memcmp(av.p, s->attrs[i].val.p, s->attrs[i].val.len) != 0) return 0;
                break;
            case AOP_SUFFIX:
                if (av.len < s->attrs[i].val.len ||
                    memcmp(av.p + av.len - s->attrs[i].val.len,
                           s->attrs[i].val.p, s->attrs[i].val.len) != 0) return 0;
                break;
            case AOP_CONTAINS:
                if (!strv_contains(av, s->attrs[i].val)) return 0;
                break;
            case AOP_WORD:
                if (!class_present(av, s->attrs[i].val)) return 0;
                break;
            case AOP_DASH:
                if (!dash_match(av, s->attrs[i].val)) return 0;
                break;
            default:
                return 0;
        }
    }
    return 1;
}

/* ---- HTML entity decode -------------------------------------------- */

static void emit_utf8(char *out, int code, int *outlen) {
    if (code < 0x80) {
        out[0] = (char)code; *outlen = 1;
    } else if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        *outlen = 2;
    } else if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        *outlen = 3;
    } else {
        out[0] = (char)(0xF0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        *outlen = 4;
    }
}

static void append_decoded(VALUE buf, const char *s, size_t len) {
    size_t start = 0, i = 0;
    while (i < len) {
        if (s[i] == '&') {
            if (i > start) rb_str_buf_cat(buf, s + start, i - start);
            size_t j = i + 1;
            size_t cap = (len - j < 10) ? (len - j) : 10;
            while (j < i + 1 + cap && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
            int matched = 0;
            if (j < len && s[j] == ';') {
                size_t elen = j - i - 1;
                const char *e = s + i + 1;
                char rep[4]; int rl = 0;
                if      (elen == 3 && memcmp(e, "amp", 3) == 0)  { rep[0] = '&';  rl = 1; }
                else if (elen == 2 && memcmp(e, "lt", 2) == 0)   { rep[0] = '<';  rl = 1; }
                else if (elen == 2 && memcmp(e, "gt", 2) == 0)   { rep[0] = '>';  rl = 1; }
                else if (elen == 4 && memcmp(e, "quot", 4) == 0) { rep[0] = '"';  rl = 1; }
                else if (elen == 4 && memcmp(e, "apos", 4) == 0) { rep[0] = '\''; rl = 1; }
                else if (elen == 4 && memcmp(e, "nbsp", 4) == 0) { rep[0] = ' ';  rl = 1; }
                else if (elen >= 2 && e[0] == '#') {
                    int code = 0;
                    if (e[1] == 'x' || e[1] == 'X') {
                        for (size_t k = 2; k < elen; k++) {
                            int c = e[k];
                            if      (c >= '0' && c <= '9') code = code * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f') code = code * 16 + (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') code = code * 16 + (c - 'A' + 10);
                            else { code = -1; break; }
                        }
                    } else {
                        for (size_t k = 1; k < elen; k++) {
                            int c = e[k];
                            if (c >= '0' && c <= '9') code = code * 10 + (c - '0');
                            else { code = -1; break; }
                        }
                    }
                    if (code > 0 && code <= 0x10FFFF) emit_utf8(rep, code, &rl);
                }
                if (rl > 0) {
                    rb_str_buf_cat(buf, rep, rl);
                    i = j + 1;
                    start = i;
                    matched = 1;
                }
            }
            if (!matched) {
                rb_str_buf_cat(buf, "&", 1);
                i++;
                start = i;
            }
        } else {
            i++;
        }
    }
    if (i > start) rb_str_buf_cat(buf, s + start, i - start);
}

static VALUE rb_str_decoded(const char *p, size_t len) {
    VALUE s = rb_str_buf_new(len);
    append_decoded(s, p, len);
    return u8(s);
}

/* ---- HTML5 void elements ------------------------------------------- */

static int is_void(strv tag) {
    static const struct { const char *n; size_t l; } voids[] = {
        {"area",4},{"base",4},{"br",2},{"col",3},{"embed",5},
        {"hr",2},{"img",3},{"input",5},{"link",4},{"meta",4},
        {"source",6},{"track",5},{"wbr",3},{NULL,0}
    };
    for (int i = 0; voids[i].n; i++) {
        if (strv_ieq_cstr(tag, voids[i].n, voids[i].l)) return 1;
    }
    return 0;
}

/* ---- field-text buffer (stack-first, grows to heap) ---------------- */

#define FB_STACK_BYTES 1024

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
    char   inline_buf[FB_STACK_BYTES];
} fbuf_t;

static void fbuf_reset(fbuf_t *b) {
    b->p = b->inline_buf;
    b->cap = FB_STACK_BYTES;
    b->len = 0;
}

static void fbuf_free(fbuf_t *b) {
    if (b->p != b->inline_buf && b->p != NULL) {
        free(b->p);
        b->p = NULL;
    }
}

static void fbuf_append(fbuf_t *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap;
        while (ncap < b->len + n) ncap *= 2;
        if (b->p == b->inline_buf) {
            char *np = (char *)malloc(ncap);
            if (!np) return;
            memcpy(np, b->p, b->len);
            b->p = np;
        } else {
            char *np = (char *)realloc(b->p, ncap);
            if (!np) return;
            b->p = np;
        }
        b->cap = ncap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* ---- schema desc parsed into native form --------------------------- */

typedef struct {
    ID      name;
    ssel    sel;
    strv    attr;          /* len==0 => text capture */
    int     type;
    int     clean;
    int     normalize_url;
    int     multi;
    /* Optional context constraint: field matches only when the
     * primary sel is reached AND `context_sel` matched an ancestor
     * (or immediate parent for child combinator). */
    int     has_context;
    int     combinator;     /* COMB_NONE | COMB_DESCENDANT | COMB_CHILD */
    ssel    context_sel;
} field_t;

typedef struct {
    ID       name;
    ssel     sel;
    int      n_fields;
    field_t  fields[MAX_FIELDS];
    VALUE    results;      /* Ruby Array */
} group_t;

typedef struct {
    strv     tag;
    int      gi_when_pushed;
    uint64_t started_fields;   /* bitmask: bit fi set => field fi captures here */
    uint64_t context_open;     /* bitmask: bit fi set => this frame matches field fi's context_sel */
    int      opens_record;
} frame_t;

typedef struct {
    const char *html;
    size_t      len;
    size_t      pos;

    frame_t     stack[MAX_STACK];
    int         sp;

    group_t     groups[MAX_GROUPS];
    int         n_groups;

    int         active_gi;
    int         record_depth;
    VALUE       record;
    fbuf_t      ftext[MAX_FIELDS];
    int         fdone[MAX_FIELDS];

    VALUE       base_url;
    const char *base_url_p;
    size_t      base_url_len;
    long        base_origin_len;
} ctx_t;

/* ---- coercion ------------------------------------------------------ */

static VALUE finalize_text(fbuf_t *b, int clean) {
    if (!clean) {
        VALUE out = rb_str_buf_new(b->len);
        append_decoded(out, b->p, b->len);
        return u8(out);
    }
    /* Squeeze whitespace first into a stack buffer, then decode entities. */
    char stack_tmp[2048];
    char *tmp = (b->len <= sizeof(stack_tmp)) ? stack_tmp : (char *)malloc(b->len);
    if (!tmp) tmp = stack_tmp;
    size_t tl = 0;
    int in_ws = 1;
    for (size_t i = 0; i < b->len; i++) {
        unsigned char ch = (unsigned char)b->p[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') {
            if (!in_ws) { tmp[tl++] = ' '; in_ws = 1; }
        } else {
            tmp[tl++] = (char)ch;
            in_ws = 0;
        }
    }
    if (tl > 0 && tmp[tl - 1] == ' ') tl--;
    VALUE out = rb_str_buf_new(tl);
    append_decoded(out, tmp, tl);
    if (tmp != stack_tmp) free(tmp);
    return u8(out);
}

static VALUE coerce_money_raw(const char *p, size_t l) {
    if (l == 0) return Qnil;
    size_t i = 0;
    while (i < l && !isdigit((unsigned char)p[i]) && p[i] != '-') i++;
    if (i >= l) return Qnil;
    size_t start = i;
    if (p[i] == '-') i++;
    while (i < l && (isdigit((unsigned char)p[i]) || p[i] == '.' || p[i] == ',')) i++;
    size_t end = i;
    if (end == start) return Qnil;

    char buf[80]; size_t bl = 0;
    int dots = 0, commas = 0;
    for (size_t k = start; k < end && bl < sizeof(buf) - 1; k++) {
        if (p[k] == '.') dots++;
        else if (p[k] == ',') commas++;
        buf[bl++] = p[k];
    }
    buf[bl] = 0;

    if (dots > 0 && commas > 0) {
        const char *last_dot = strrchr(buf, '.');
        const char *last_comma = strrchr(buf, ',');
        char tmp[80]; size_t ti = 0;
        if (last_dot > last_comma) {
            for (size_t k = 0; k < bl; k++) if (buf[k] != ',') tmp[ti++] = buf[k];
        } else {
            for (size_t k = 0; k < bl; k++) {
                if (buf[k] == '.') continue;
                tmp[ti++] = (buf[k] == ',') ? '.' : buf[k];
            }
        }
        tmp[ti] = 0; memcpy(buf, tmp, ti + 1); bl = ti;
    } else if (commas > 1) {
        char tmp[80]; size_t ti = 0;
        for (size_t k = 0; k < bl; k++) if (buf[k] != ',') tmp[ti++] = buf[k];
        tmp[ti] = 0; memcpy(buf, tmp, ti + 1); bl = ti;
    } else if (commas == 1) {
        char *cp = strchr(buf, ',');
        size_t rest = bl - (size_t)(cp - buf) - 1;
        int all_d = 1;
        for (size_t k = 0; k < rest; k++) if (!isdigit((unsigned char)cp[1 + k])) { all_d = 0; break; }
        if (all_d && rest == 3) {
            char tmp[80]; size_t ti = 0;
            for (size_t k = 0; k < bl; k++) if (buf[k] != ',') tmp[ti++] = buf[k];
            tmp[ti] = 0; memcpy(buf, tmp, ti + 1); bl = ti;
        } else {
            for (size_t k = 0; k < bl; k++) if (buf[k] == ',') buf[k] = '.';
        }
    } else if (dots > 1) {
        const char *cp = strrchr(buf, '.');
        size_t rest = bl - (size_t)(cp - buf) - 1;
        if (rest == 3) {
            char tmp[80]; size_t ti = 0;
            for (size_t k = 0; k < bl; k++) if (buf[k] != '.') tmp[ti++] = buf[k];
            tmp[ti] = 0; memcpy(buf, tmp, ti + 1); bl = ti;
        }
    }
    double d = strtod(buf, NULL);
    return DBL2NUM(d);
}

static VALUE coerce_money_fbuf(fbuf_t *b)  { return coerce_money_raw(b->p, b->len); }
static VALUE coerce_money_strv(strv v)     { return coerce_money_raw(v.p, v.len); }

static VALUE url_absolute(const char *p, size_t l, ctx_t *c) {
    VALUE s = rb_str_buf_new(l);
    append_decoded(s, p, l);
    long sl = RSTRING_LEN(s);
    if (sl == 0) return Qnil;
    const char *sp = RSTRING_PTR(s);

    if (sl >= 7 && memcmp(sp, "http://", 7) == 0) return u8(s);
    if (sl >= 8 && memcmp(sp, "https://", 8) == 0) return u8(s);

    if (NIL_P(c->base_url)) return u8(s);

    if (sl >= 2 && sp[0] == '/' && sp[1] == '/') {
        long scheme_end = 0;
        while (scheme_end < (long)c->base_url_len && c->base_url_p[scheme_end] != ':') scheme_end++;
        VALUE out = rb_str_buf_new(scheme_end + 1 + sl);
        rb_str_buf_cat(out, c->base_url_p, scheme_end);
        rb_str_buf_cat(out, ":", 1);
        rb_str_buf_cat(out, sp, sl);
        return u8(out);
    }

    if (sl >= 1 && sp[0] == '/') {
        VALUE out = rb_str_buf_new(c->base_origin_len + sl);
        rb_str_buf_cat(out, c->base_url_p, c->base_origin_len);
        rb_str_buf_cat(out, sp, sl);
        return u8(out);
    }

    VALUE uri_mod = rb_const_get(rb_cObject, rb_intern("URI"));
    VALUE joined  = rb_funcall(uri_mod, rb_intern("join"), 2, c->base_url, u8(s));
    return rb_funcall(joined, rb_intern("to_s"), 0);
}

static VALUE field_array_for(ctx_t *c, ID name) {
    VALUE sym = ID2SYM(name);
    VALUE arr = rb_hash_lookup(c->record, sym);
    if (!RB_TYPE_P(arr, T_ARRAY)) {
        arr = rb_ary_new();
        rb_hash_aset(c->record, sym, arr);
    }
    return arr;
}

static void assign_field_value(ctx_t *c, field_t *f, VALUE val) {
    if (NIL_P(val)) return;
    if (f->multi) {
        VALUE arr = field_array_for(c, f->name);
        rb_ary_push(arr, val);
    } else {
        rb_hash_aset(c->record, ID2SYM(f->name), val);
    }
}

static void finalize_field(ctx_t *c, int gi, int fi) {
    field_t *f = &c->groups[gi].fields[fi];
    fbuf_t *b = &c->ftext[fi];
    VALUE val = Qnil;
    switch (f->type) {
        case FT_TEXT:
            val = finalize_text(b, f->clean);
            break;
        case FT_MONEY:
            val = coerce_money_fbuf(b);
            break;
        case FT_URL:
            val = f->normalize_url ? url_absolute(b->p, b->len, c) : finalize_text(b, 0);
            break;
        case FT_INTEGER: {
            char tmp[64]; size_t tl = b->len < 63 ? b->len : 63;
            size_t ti = 0;
            for (size_t k = 0; k < tl; k++) {
                char ch = b->p[k];
                if ((ch >= '0' && ch <= '9') || ch == '-') tmp[ti++] = ch;
            }
            if (ti == 0) { val = Qnil; break; }
            tmp[ti] = 0;
            val = INT2NUM(atoi(tmp));
            break;
        }
        case FT_FLOAT: {
            char tmp[64]; size_t tl = b->len < 63 ? b->len : 63;
            size_t ti = 0;
            for (size_t k = 0; k < tl; k++) {
                char ch = b->p[k];
                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') tmp[ti++] = ch;
            }
            if (ti == 0) { val = Qnil; break; }
            tmp[ti] = 0;
            val = DBL2NUM(strtod(tmp, NULL));
            break;
        }
        default:
            val = finalize_text(b, f->clean);
    }
    assign_field_value(c, f, val);
    if (f->multi) {
        fbuf_reset(b); /* ready for the next match in this record */
    } else {
        c->fdone[fi] = 1;
    }
}

/* ---- tokenizer ----------------------------------------------------- */

static void skip_comment(ctx_t *c) {
    c->pos += 4;
    while (c->pos + 2 < c->len) {
        if (c->html[c->pos] == '-' && c->html[c->pos + 1] == '-' && c->html[c->pos + 2] == '>') {
            c->pos += 3;
            return;
        }
        c->pos++;
    }
    c->pos = c->len;
}

static void skip_until_gt(ctx_t *c) {
    while (c->pos < c->len && c->html[c->pos] != '>') c->pos++;
    if (c->pos < c->len) c->pos++;
}

static void skip_raw_until(ctx_t *c, const char *name, size_t nlen) {
    while (c->pos < c->len) {
        const char *next_lt = (const char *)memchr(c->html + c->pos, '<', c->len - c->pos);
        if (!next_lt) { c->pos = c->len; return; }
        size_t p = (size_t)(next_lt - c->html);
        if (p + 1 + nlen < c->len && c->html[p + 1] == '/' && strncasecmp(c->html + p + 2, name, nlen) == 0) {
            char after = (p + 2 + nlen < c->len) ? c->html[p + 2 + nlen] : '\0';
            if (after == '>' || after == ' ' || after == '\t' || after == '\n' || after == '/' || after == '\r') {
                c->pos = p;
                skip_until_gt(c);
                return;
            }
        }
        c->pos = p + 1;
    }
}

static inline int is_name_start(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static inline int is_name_char(int ch)  { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == ':'; }
static inline int is_ws(int ch)         { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }

static inline void emit_text_span(ctx_t *c, size_t start, size_t end) {
    if (start >= end) return;
    if (c->active_gi < 0) return;
    /* Union of all "started" fields across frames inside the current record. */
    uint64_t open = 0;
    for (int i = c->record_depth; i < c->sp; i++) open |= c->stack[i].started_fields;
    if (open == 0) return;
    const char *src = c->html + start;
    size_t span = end - start;
    /* Iterate set bits */
    while (open) {
        int fi = __builtin_ctzll(open);
        open &= open - 1;
        field_t *f = &c->groups[c->active_gi].fields[fi];
        if (f->multi || !c->fdone[fi]) {
            fbuf_append(&c->ftext[fi], src, span);
        }
    }
}

static void open_record(ctx_t *c, int gi) {
    c->active_gi    = gi;
    c->record_depth = c->sp;
    c->record       = rb_hash_new();
    group_t *g = &c->groups[gi];
    for (int i = 0; i < g->n_fields; i++) {
        fbuf_reset(&c->ftext[i]);
        c->fdone[i] = 0;
    }
}

static void close_record(ctx_t *c) {
    if (c->active_gi < 0) return;
    group_t *g = &c->groups[c->active_gi];
    for (int i = 0; i < g->n_fields; i++) {
        if (!c->fdone[i] && c->ftext[i].len > 0) finalize_field(c, c->active_gi, i);
    }
    rb_ary_push(g->results, c->record);
    c->record = Qnil;
    c->active_gi = -1;
}

static void handle_start_tag(ctx_t *c) {
    c->pos++;
    size_t tag_start = c->pos;
    while (c->pos < c->len && is_name_char((unsigned char)c->html[c->pos])) c->pos++;
    strv tag = { c->html + tag_start, c->pos - tag_start };
    if (tag.len == 0) return;

    strv attrs[MAX_ATTRS][2];
    int n_attrs = 0;
    strv cls = { NULL, 0 };
    strv id_attr = { NULL, 0 };
    int self_closing = 0;

    while (c->pos < c->len) {
        while (c->pos < c->len && is_ws((unsigned char)c->html[c->pos])) c->pos++;
        if (c->pos >= c->len) break;
        char ch = c->html[c->pos];
        if (ch == '>') { c->pos++; break; }
        if (ch == '/' && c->pos + 1 < c->len && c->html[c->pos + 1] == '>') {
            self_closing = 1;
            c->pos += 2;
            break;
        }
        size_t an_s = c->pos;
        while (c->pos < c->len) {
            unsigned char nc = (unsigned char)c->html[c->pos];
            if (nc == '=' || nc == '>' || nc == '/' || is_ws(nc)) break;
            c->pos++;
        }
        strv aname = { c->html + an_s, c->pos - an_s };
        strv aval  = { NULL, 0 };

        while (c->pos < c->len && is_ws((unsigned char)c->html[c->pos])) c->pos++;
        if (c->pos < c->len && c->html[c->pos] == '=') {
            c->pos++;
            while (c->pos < c->len && is_ws((unsigned char)c->html[c->pos])) c->pos++;
            if (c->pos < c->len) {
                char q = c->html[c->pos];
                if (q == '"' || q == '\'') {
                    c->pos++;
                    size_t av_s = c->pos;
                    while (c->pos < c->len && c->html[c->pos] != q) c->pos++;
                    aval.p = c->html + av_s;
                    aval.len = c->pos - av_s;
                    if (c->pos < c->len) c->pos++;
                } else {
                    size_t av_s = c->pos;
                    while (c->pos < c->len && !is_ws((unsigned char)c->html[c->pos]) && c->html[c->pos] != '>') c->pos++;
                    aval.p = c->html + av_s;
                    aval.len = c->pos - av_s;
                }
            }
        }
        if (aname.len > 0 && n_attrs < MAX_ATTRS) {
            attrs[n_attrs][0] = aname;
            attrs[n_attrs][1] = aval;
            n_attrs++;
        }
        if (aname.len == 5 && strv_ieq_cstr(aname, "class", 5)) cls = aval;
        else if (aname.len == 2 && strv_ieq_cstr(aname, "id", 2)) id_attr = aval;
    }

    int void_el = is_void(tag);
    int will_push = !void_el && !self_closing;

    if (c->active_gi < 0) {
        for (int gi = 0; gi < c->n_groups; gi++) {
            if (sel_match(&c->groups[gi].sel, tag, cls, id_attr, attrs, n_attrs)) {
                if (will_push) open_record(c, gi);
                break;
            }
        }
    }

    uint64_t my_fields  = 0;
    uint64_t my_context = 0;
    if (c->active_gi >= 0) {
        group_t *g = &c->groups[c->active_gi];
        int nfields = g->n_fields;
        if (nfields > 64) nfields = 64;

        /* First pass: which fields' context selectors does this frame match? */
        for (int fi = 0; fi < nfields; fi++) {
            field_t *f = &g->fields[fi];
            if (!f->has_context) continue;
            if (sel_match(&f->context_sel, tag, cls, id_attr, attrs, n_attrs)) {
                my_context |= ((uint64_t)1 << fi);
            }
        }

        /* Second pass: primary matching, gated by context constraint. */
        for (int fi = 0; fi < nfields; fi++) {
            field_t *f = &g->fields[fi];
            if (!f->multi && c->fdone[fi]) continue;
            if (!sel_match(&f->sel, tag, cls, id_attr, attrs, n_attrs)) continue;

            if (f->has_context) {
                uint64_t open = 0;
                if (f->combinator == COMB_DESCENDANT) {
                    for (int i = c->record_depth; i < c->sp; i++) {
                        open |= c->stack[i].context_open;
                    }
                } else if (f->combinator == COMB_CHILD) {
                    if (c->sp > 0) open = c->stack[c->sp - 1].context_open;
                }
                if (!(open & ((uint64_t)1 << fi))) continue;
            }
            if (f->attr.len > 0) {
                strv av = { NULL, 0 };
                int found = 0;
                for (int a = 0; a < n_attrs; a++) {
                    if (strv_ieq(attrs[a][0], f->attr)) {
                        av = attrs[a][1];
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    VALUE val = Qnil;
                    switch (f->type) {
                        case FT_URL:
                            val = f->normalize_url ? url_absolute(av.p, av.len, c)
                                                   : rb_str_decoded(av.p, av.len);
                            break;
                        case FT_MONEY: val = coerce_money_strv(av); break;
                        default:       val = rb_str_decoded(av.p, av.len); break;
                    }
                    assign_field_value(c, f, val);
                    if (!f->multi) c->fdone[fi] = 1;
                }
            } else {
                if (will_push) {
                    fbuf_reset(&c->ftext[fi]);
                    my_fields |= ((uint64_t)1 << fi);
                }
            }
        }
    }

    if (strv_ieq_cstr(tag, "script", 6)) {
        skip_raw_until(c, "script", 6);
        return;
    }
    if (strv_ieq_cstr(tag, "style", 5)) {
        skip_raw_until(c, "style", 5);
        return;
    }

    if (will_push) {
        if (c->sp < MAX_STACK) {
            frame_t *fr = &c->stack[c->sp];
            fr->tag = tag;
            fr->gi_when_pushed = c->active_gi;
            fr->started_fields = my_fields;
            fr->context_open   = my_context;
            fr->opens_record = (c->active_gi >= 0 && c->record_depth == c->sp) ? 1 : 0;
            c->sp++;
        }
    }
}

static void handle_end_tag(ctx_t *c) {
    c->pos += 2;
    size_t name_s = c->pos;
    while (c->pos < c->len && is_name_char((unsigned char)c->html[c->pos])) c->pos++;
    strv name = { c->html + name_s, c->pos - name_s };
    skip_until_gt(c);

    if (c->sp == 0 || name.len == 0) return;

    int found_at = -1;
    for (int i = c->sp - 1; i >= 0; i--) {
        if (strv_ieq(c->stack[i].tag, name)) { found_at = i; break; }
    }
    if (found_at < 0) return;

    while (c->sp > found_at) {
        c->sp--;
        frame_t *fr = &c->stack[c->sp];
        uint64_t open = fr->started_fields;
        while (open && c->active_gi >= 0) {
            int fi = __builtin_ctzll(open);
            open &= open - 1;
            field_t *f = &c->groups[c->active_gi].fields[fi];
            if (f->multi || !c->fdone[fi]) {
                finalize_field(c, c->active_gi, fi);
            }
        }
        if (fr->opens_record) {
            close_record(c);
        }
    }
}

static void scan(ctx_t *c) {
    while (c->pos < c->len) {
        size_t text_start = c->pos;
        const char *next_lt = (const char *)memchr(c->html + c->pos, '<', c->len - c->pos);
        size_t lt = next_lt ? (size_t)(next_lt - c->html) : c->len;
        emit_text_span(c, text_start, lt);
        c->pos = lt;
        if (c->pos >= c->len) break;

        if (c->pos + 3 < c->len &&
            c->html[c->pos + 1] == '!' && c->html[c->pos + 2] == '-' && c->html[c->pos + 3] == '-') {
            skip_comment(c);
            continue;
        }
        if (c->pos + 1 < c->len && c->html[c->pos + 1] == '!') {
            skip_until_gt(c);
            continue;
        }
        if (c->pos + 1 < c->len && c->html[c->pos + 1] == '/') {
            handle_end_tag(c);
            continue;
        }
        if (c->pos + 1 < c->len && is_name_start((unsigned char)c->html[c->pos + 1])) {
            handle_start_tag(c);
            continue;
        }
        emit_text_span(c, c->pos, c->pos + 1);
        c->pos++;
    }
}

/* ---- descriptor parsing -------------------------------------------- */

static ID id_text, id_money, id_url, id_integer, id_float;

/* New selector format from Ruby:
 *   sel = [tag_or_nil, classes_array, id_or_nil, attrs_array]
 *   attrs_array = [[name_str, op_str_or_nil, val_str_or_nil], ...]
 *   op_str ∈ {"=", "*=", "^=", "$=", "~=", "|=", nil}
 */
static int parse_attr_op(VALUE op_v) {
    if (NIL_P(op_v)) return AOP_EXISTS;
    if (!RB_TYPE_P(op_v, T_STRING)) return -1;
    const char *p = RSTRING_PTR(op_v);
    long l = RSTRING_LEN(op_v);
    if (l == 1 && p[0] == '=') return AOP_EQ;
    if (l == 2 && p[1] == '=') {
        switch (p[0]) {
            case '*': return AOP_CONTAINS;
            case '^': return AOP_PREFIX;
            case '$': return AOP_SUFFIX;
            case '~': return AOP_WORD;
            case '|': return AOP_DASH;
        }
    }
    return -1;
}

static int parse_selector_value(VALUE sel_v, ssel *out) {
    memset(out, 0, sizeof(*out));
    if (!RB_TYPE_P(sel_v, T_ARRAY) || RARRAY_LEN(sel_v) < 4) return 0;

    VALUE tag_v = rb_ary_entry(sel_v, 0);
    if (NIL_P(tag_v)) { out->tag.p = NULL; out->tag.len = 0; }
    else {
        if (!RB_TYPE_P(tag_v, T_STRING)) return 0;
        out->tag.p   = RSTRING_PTR(tag_v);
        out->tag.len = (size_t)RSTRING_LEN(tag_v);
    }

    VALUE classes_v = rb_ary_entry(sel_v, 1);
    if (!RB_TYPE_P(classes_v, T_ARRAY)) return 0;
    long nc = RARRAY_LEN(classes_v);
    if (nc > MAX_CLASSES_PER_SEL) return 0;
    out->n_classes = (int)nc;
    for (int i = 0; i < out->n_classes; i++) {
        VALUE c = rb_ary_entry(classes_v, i);
        if (!RB_TYPE_P(c, T_STRING)) return 0;
        out->classes[i].p   = RSTRING_PTR(c);
        out->classes[i].len = (size_t)RSTRING_LEN(c);
    }

    VALUE id_v = rb_ary_entry(sel_v, 2);
    if (NIL_P(id_v)) { out->id.p = NULL; out->id.len = 0; }
    else {
        if (!RB_TYPE_P(id_v, T_STRING)) return 0;
        out->id.p   = RSTRING_PTR(id_v);
        out->id.len = (size_t)RSTRING_LEN(id_v);
    }

    VALUE attrs_v = rb_ary_entry(sel_v, 3);
    if (!RB_TYPE_P(attrs_v, T_ARRAY)) return 0;
    long na = RARRAY_LEN(attrs_v);
    if (na > MAX_ATTRMATCH_PER_SEL) return 0;
    out->n_attrs = (int)na;
    for (int i = 0; i < out->n_attrs; i++) {
        VALUE a = rb_ary_entry(attrs_v, i);
        if (!RB_TYPE_P(a, T_ARRAY) || RARRAY_LEN(a) < 3) return 0;
        VALUE n = rb_ary_entry(a, 0);
        VALUE o = rb_ary_entry(a, 1);
        VALUE v = rb_ary_entry(a, 2);
        if (!RB_TYPE_P(n, T_STRING)) return 0;
        int op = parse_attr_op(o);
        if (op < 0) return 0;
        out->attrs[i].name.p   = RSTRING_PTR(n);
        out->attrs[i].name.len = (size_t)RSTRING_LEN(n);
        out->attrs[i].op = op;
        if (op == AOP_EXISTS) {
            out->attrs[i].val.p = NULL;
            out->attrs[i].val.len = 0;
        } else {
            if (!RB_TYPE_P(v, T_STRING)) return 0;
            out->attrs[i].val.p   = RSTRING_PTR(v);
            out->attrs[i].val.len = (size_t)RSTRING_LEN(v);
        }
    }

    return 1;
}

/* Descriptor format (from Ruby):
 *
 *   desc   = [group, group, ...]
 *   group  = [name_sym, sel, fields_array]
 *   field  = [name_sym, sel, attr_str_or_nil, type_sym, clean_bool,
 *             normalize_url_bool, multi_bool,
 *             context_sel_or_nil, combinator_or_nil]
 *   sel    = [tag_or_nil, classes_array, id_or_nil, attrs_array]
 *   combinator ∈ { nil, "descendant", "child" }
 */
static int parse_descriptor(VALUE desc, ctx_t *c) {
    if (!RB_TYPE_P(desc, T_ARRAY)) return 0;
    long n = RARRAY_LEN(desc);
    if (n > MAX_GROUPS) n = MAX_GROUPS;
    c->n_groups = (int)n;

    for (int gi = 0; gi < c->n_groups; gi++) {
        VALUE gd = rb_ary_entry(desc, gi);
        if (!RB_TYPE_P(gd, T_ARRAY) || RARRAY_LEN(gd) < 3) return 0;

        group_t *g = &c->groups[gi];
        VALUE name_sym = rb_ary_entry(gd, 0);
        if (!SYMBOL_P(name_sym)) return 0;
        g->name = SYM2ID(name_sym);

        if (!parse_selector_value(rb_ary_entry(gd, 1), &g->sel)) return 0;

        VALUE fields_v = rb_ary_entry(gd, 2);
        if (!RB_TYPE_P(fields_v, T_ARRAY)) return 0;
        long nf = RARRAY_LEN(fields_v);
        if (nf > MAX_FIELDS) nf = MAX_FIELDS;
        g->n_fields = (int)nf;

        for (int fi = 0; fi < g->n_fields; fi++) {
            VALUE fd = rb_ary_entry(fields_v, fi);
            if (!RB_TYPE_P(fd, T_ARRAY) || RARRAY_LEN(fd) < 7) return 0;
            field_t *f = &g->fields[fi];
            VALUE fname = rb_ary_entry(fd, 0);
            if (!SYMBOL_P(fname)) return 0;
            f->name = SYM2ID(fname);
            if (!parse_selector_value(rb_ary_entry(fd, 1), &f->sel)) return 0;
            VALUE fattr = rb_ary_entry(fd, 2);
            if (NIL_P(fattr)) { f->attr.p = NULL; f->attr.len = 0; }
            else {
                if (!RB_TYPE_P(fattr, T_STRING)) return 0;
                f->attr.p   = RSTRING_PTR(fattr);
                f->attr.len = (size_t)RSTRING_LEN(fattr);
            }
            VALUE ftype = rb_ary_entry(fd, 3);
            if (!SYMBOL_P(ftype)) return 0;
            ID tid = SYM2ID(ftype);
            if      (tid == id_money)   f->type = FT_MONEY;
            else if (tid == id_url)     f->type = FT_URL;
            else if (tid == id_integer) f->type = FT_INTEGER;
            else if (tid == id_float)   f->type = FT_FLOAT;
            else                        f->type = FT_TEXT;
            f->clean         = RTEST(rb_ary_entry(fd, 4)) ? 1 : 0;
            f->normalize_url = RTEST(rb_ary_entry(fd, 5)) ? 1 : 0;
            f->multi         = RTEST(rb_ary_entry(fd, 6)) ? 1 : 0;

            /* Optional context + combinator (positions 7, 8). */
            f->has_context = 0;
            f->combinator  = COMB_NONE;
            memset(&f->context_sel, 0, sizeof(f->context_sel));
            if (RARRAY_LEN(fd) >= 9) {
                VALUE ctx = rb_ary_entry(fd, 7);
                VALUE comb = rb_ary_entry(fd, 8);
                if (!NIL_P(ctx) && !NIL_P(comb)) {
                    if (!parse_selector_value(ctx, &f->context_sel)) return 0;
                    if (!RB_TYPE_P(comb, T_STRING)) return 0;
                    const char *cp = RSTRING_PTR(comb);
                    long cl = RSTRING_LEN(comb);
                    if      (cl == 10 && memcmp(cp, "descendant", 10) == 0) f->combinator = COMB_DESCENDANT;
                    else if (cl == 5  && memcmp(cp, "child", 5) == 0)       f->combinator = COMB_CHILD;
                    else return 0;
                    f->has_context = 1;
                }
            }
        }
        g->results = rb_ary_new();
    }
    return 1;
}

static long compute_origin_len(const char *p, long l) {
    long i = 0;
    while (i < l && p[i] != ':') i++;
    if (i + 2 >= l || p[i + 1] != '/' || p[i + 2] != '/') return l;
    i += 3;
    while (i < l && p[i] != '/') i++;
    return i;
}

/* ---- entrypoint ---------------------------------------------------- */

static VALUE scrapetor_extract(VALUE self, VALUE html_v, VALUE desc_v, VALUE base_url_v) {
    (void)self;
    Check_Type(html_v, T_STRING);

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.html       = RSTRING_PTR(html_v);
    ctx.len        = (size_t)RSTRING_LEN(html_v);
    ctx.pos        = 0;
    ctx.sp         = 0;
    ctx.active_gi  = -1;
    ctx.record     = Qnil;
    ctx.base_url   = NIL_P(base_url_v) ? Qnil : base_url_v;
    if (!NIL_P(ctx.base_url)) {
        Check_Type(ctx.base_url, T_STRING);
        ctx.base_url_p      = RSTRING_PTR(ctx.base_url);
        ctx.base_url_len    = (size_t)RSTRING_LEN(ctx.base_url);
        ctx.base_origin_len = compute_origin_len(ctx.base_url_p, (long)ctx.base_url_len);
    }
    for (int i = 0; i < MAX_FIELDS; i++) fbuf_reset(&ctx.ftext[i]);

    if (!parse_descriptor(desc_v, &ctx)) {
        for (int i = 0; i < MAX_FIELDS; i++) fbuf_free(&ctx.ftext[i]);
        rb_raise(rb_eArgError, "scrapetor_native: invalid schema descriptor");
    }

    scan(&ctx);

    if (ctx.active_gi >= 0) {
        group_t *g = &ctx.groups[ctx.active_gi];
        for (int i = 0; i < g->n_fields; i++) {
            if (!ctx.fdone[i] && ctx.ftext[i].len > 0) finalize_field(&ctx, ctx.active_gi, i);
        }
        rb_ary_push(g->results, ctx.record);
    }

    VALUE result = rb_hash_new();
    for (int gi = 0; gi < ctx.n_groups; gi++) {
        rb_hash_aset(result, ID2SYM(ctx.groups[gi].name), ctx.groups[gi].results);
    }

    for (int i = 0; i < MAX_FIELDS; i++) fbuf_free(&ctx.ftext[i]);

    return result;
}

/* ---- module init --------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
void Init_scrapetor_native(void) {
    id_text    = rb_intern("text");
    id_money   = rb_intern("money");
    id_url     = rb_intern("url");
    id_integer = rb_intern("integer");
    id_float   = rb_intern("float");
    enc_utf8   = rb_utf8_encoding();

    VALUE mod_scrapetor = rb_define_module("Scrapetor");
    VALUE mod_native    = rb_define_module_under(mod_scrapetor, "Native");
    rb_define_singleton_method(mod_native, "extract", scrapetor_extract, 3);

    /* Register the native arena-DOM module too. */
    extern void Init_scrapetor_dom(VALUE);
    Init_scrapetor_dom(mod_native);
}
