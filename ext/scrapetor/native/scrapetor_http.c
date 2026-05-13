/*
 * scrapetor_http.c
 *
 * Native HTTP/2-capable fetch layer. Wraps libcurl's easy interface
 * with sensible scraping defaults: HTTP/2 over TLS, automatic
 * Accept-Encoding (gzip + brotli + zstd when the linked libcurl was
 * built with them), connection reuse via a per-thread persistent
 * handle, redirect following with a cap, and total-time timeout.
 *
 * Build-time conditional. If pkg-config can't find libcurl (or the
 * caller passes --without-libcurl to extconf), this whole file
 * collapses to a stub that raises a clear error at fetch time —
 * Scrapetor still loads, only the HTTP surface is unavailable.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <ruby/thread.h>

#ifdef HAVE_LIBCURL

#include <curl/curl.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef HAVE_BROTLI
#include <brotli/decode.h>
#endif

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

extern rb_encoding *enc_utf8;

/* ---- Accept-Encoding negotiation --------------------------------- *
 * Returns the comma-separated list of content codings this build can
 * decode. We own decompression end-to-end — CURLOPT_ACCEPT_ENCODING is
 * intentionally left unset so libcurl doesn't reject responses whose
 * encoding it wasn't compiled for. */
static const char *scrap_accept_encoding(void) {
    static char cached[128];
    static int  inited = 0;
    if (inited) return cached;
    cached[0] = 0;
    int first = 1;
#ifdef HAVE_ZLIB
    { strcat(cached, first ? "gzip, deflate" : ", gzip, deflate"); first = 0; }
#endif
#ifdef HAVE_BROTLI
    { strcat(cached, first ? "br" : ", br"); first = 0; }
#endif
#ifdef HAVE_ZSTD
    { strcat(cached, first ? "zstd" : ", zstd"); first = 0; }
#endif
    if (first) {
        /* No codecs linked at all — advertise identity so servers know
         * not to compress. */
        strcpy(cached, "identity");
    }
    inited = 1;
    return cached;
}

/* ---- in-process zlib / brotli / zstd decoders -------------------- */

#ifdef HAVE_ZLIB
/* `gzip` and `deflate`. window_bits selects which: 31 = gzip wrapper,
 * 15 = zlib wrapper, -15 = raw deflate. The 47 path auto-detects gzip
 * vs zlib, which is what we want since some servers send Content-
 * Encoding: deflate with the zlib wrapper and others without. */
static int scrap_zlib_decode(const char *in, size_t in_len,
                             int window_bits,
                             char **out, size_t *out_len) {
    z_stream s; memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, window_bits) != Z_OK) return 0;
    size_t cap = in_len * 4 + 4096;
    char  *buf = (char *)malloc(cap);
    size_t total = 0;
    s.next_in  = (Bytef *)in;
    s.avail_in = (uInt)in_len;
    while (1) {
        if (cap - total < 4096) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        s.next_out  = (Bytef *)(buf + total);
        s.avail_out = (uInt)(cap - total);
        int r = inflate(&s, Z_NO_FLUSH);
        total = cap - s.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) { inflateEnd(&s); free(buf); return 0; }
        if (s.avail_in == 0 && s.avail_out > 0) break;
    }
    inflateEnd(&s);
    *out = buf; *out_len = total;
    return 1;
}
#endif

/* ---- in-process brotli / zstd decoders --------------------------- */

#ifdef HAVE_BROTLI
static int scrap_brotli_decode(const char *in, size_t in_len,
                               char **out, size_t *out_len) {
    BrotliDecoderState *st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st) return 0;
    size_t cap = in_len * 4 + 1024;
    char  *buf = (char *)malloc(cap);
    size_t total = 0;
    const uint8_t *next_in = (const uint8_t *)in;
    size_t avail_in = in_len;
    BrotliDecoderResult r;
    do {
        uint8_t *next_out = (uint8_t *)(buf + total);
        size_t   avail_out = cap - total;
        r = BrotliDecoderDecompressStream(st, &avail_in, &next_in,
                                          &avail_out, &next_out, NULL);
        total = (size_t)((char *)next_out - buf);
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
    } while (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
             r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT);
    BrotliDecoderDestroyInstance(st);
    if (r != BROTLI_DECODER_RESULT_SUCCESS) { free(buf); return 0; }
    *out = buf; *out_len = total;
    return 1;
}
#endif

#ifdef HAVE_ZSTD
static int scrap_zstd_decode(const char *in, size_t in_len,
                             char **out, size_t *out_len) {
    /* Use streaming zstd so we don't have to trust the frame's
     * declared size. */
    ZSTD_DStream *zds = ZSTD_createDStream();
    if (!zds) return 0;
    ZSTD_initDStream(zds);
    size_t cap = ZSTD_DStreamOutSize();
    if (cap < in_len * 4) cap = in_len * 4 + 4096;
    char  *buf = (char *)malloc(cap);
    size_t total = 0;
    ZSTD_inBuffer  zin  = { in, in_len, 0 };
    while (zin.pos < zin.size) {
        if (cap - total < ZSTD_DStreamOutSize()) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        ZSTD_outBuffer zout = { buf + total, cap - total, 0 };
        size_t r = ZSTD_decompressStream(zds, &zout, &zin);
        if (ZSTD_isError(r)) { ZSTD_freeDStream(zds); free(buf); return 0; }
        total += zout.pos;
        if (r == 0) break;  /* frame complete */
    }
    ZSTD_freeDStream(zds);
    *out = buf; *out_len = total;
    return 1;
}
#endif

/* ---- per-thread curl handle pool ---------------------------------- *
 * Re-creating an easy handle costs ~30 µs and discards connection
 * cache. Holding one handle per OS thread (via pthread_specific) lets
 * back-to-back fetches against the same host reuse the TLS/HTTP-2
 * session. Cleared automatically on thread exit. */

static pthread_key_t  g_curl_tls_key;
static pthread_once_t g_curl_tls_once = PTHREAD_ONCE_INIT;

static void curl_tls_dtor(void *p) {
    if (p) curl_easy_cleanup((CURL *)p);
}
static void curl_tls_init(void) {
    pthread_key_create(&g_curl_tls_key, curl_tls_dtor);
}

static CURL *get_thread_curl(void) {
    pthread_once(&g_curl_tls_once, curl_tls_init);
    CURL *h = (CURL *)pthread_getspecific(g_curl_tls_key);
    if (!h) {
        h = curl_easy_init();
        pthread_setspecific(g_curl_tls_key, h);
    } else {
        curl_easy_reset(h);
    }
    return h;
}

/* ---- response buffer --------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static size_t buf_append(buf_t *b, const char *src, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap == 0 ? 16 * 1024 : b->cap * 2;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = (char *)realloc(b->data, nc);
        if (!p) return 0;
        b->data = p; b->cap = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static size_t cb_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    buf_t *b = (buf_t *)userdata;
    return buf_append(b, ptr, size * nmemb);
}

static size_t cb_header(char *ptr, size_t size, size_t nmemb, void *userdata) {
    buf_t *b = (buf_t *)userdata;
    return buf_append(b, ptr, size * nmemb);
}

/* ---- fetch context ----------------------------------------------- *
 * Built under GVL, then handed to the no-GVL worker which runs
 * curl_easy_perform. */

typedef struct {
    CURL              *handle;
    buf_t              body;
    buf_t              headers;
    struct curl_slist *req_headers;  /* freed by caller */
    CURLcode           rc;
} fetch_ctx_t;

static void *do_fetch_nogvl(void *arg) {
    fetch_ctx_t *fc = (fetch_ctx_t *)arg;
    fc->rc = curl_easy_perform(fc->handle);
    return NULL;
}

/* Parse a HTTP header blob ("HTTP/2 200\r\nHeader: value\r\n...") into
 * a Ruby Hash. Multi-value headers get concatenated. Status lines are
 * filtered out so the Hash only carries response headers. */
static VALUE parse_headers_blob(const char *data, size_t len) {
    VALUE h = rb_hash_new();
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n') i++;
        size_t line_end = i;
        if (line_end > line_start && data[line_end - 1] == '\r') line_end--;
        if (i < len) i++;
        if (line_end == line_start) continue;

        /* Skip the "HTTP/x.y NNN ..." status line; curl emits one per
         * redirect step. Real headers always contain ':'. */
        size_t colon = (size_t)-1;
        for (size_t k = line_start; k < line_end; k++) {
            if (data[k] == ':') { colon = k; break; }
        }
        if (colon == (size_t)-1) continue;

        size_t name_s = line_start;
        size_t name_e = colon;
        size_t val_s = colon + 1;
        while (val_s < line_end && (data[val_s] == ' ' || data[val_s] == '\t')) val_s++;
        size_t val_e = line_end;

        VALUE name = rb_str_new(data + name_s, (long)(name_e - name_s));
        VALUE val  = rb_str_new(data + val_s,  (long)(val_e  - val_s));
        rb_enc_associate(name, enc_utf8);
        rb_enc_associate(val,  enc_utf8);
        /* Header names are ASCII case-insensitive; downcase for lookup
         * ergonomics on the Ruby side. */
        rb_funcall(name, rb_intern("downcase!"), 0);
        VALUE existing = rb_hash_lookup(h, name);
        if (NIL_P(existing)) {
            rb_hash_aset(h, name, val);
        } else {
            VALUE both = rb_str_dup(existing);
            rb_str_cat_cstr(both, ", ");
            rb_str_append(both, val);
            rb_hash_aset(h, name, both);
        }
    }
    return h;
}

static VALUE scrap_http_get(int argc, VALUE *argv, VALUE self) {
    (void)self;
    VALUE url_v, opts_v;
    rb_scan_args(argc, argv, "11", &url_v, &opts_v);
    Check_Type(url_v, T_STRING);

    long timeout_ms      = 30000;
    int  follow          = 1;
    long max_redirs      = 10;
    const char *ua       = "scrapetor/0.1 (libcurl)";
    VALUE headers_v      = Qnil;
    int  insecure        = 0;

    if (!NIL_P(opts_v)) {
        Check_Type(opts_v, T_HASH);
        VALUE v;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("timeout_ms")));
        if (!NIL_P(v)) timeout_ms = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("follow_redirects")));
        if (!NIL_P(v)) follow = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("max_redirects")));
        if (!NIL_P(v)) max_redirs = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("user_agent")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); ua = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("headers")));
        if (!NIL_P(v)) { Check_Type(v, T_HASH); headers_v = v; }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("insecure")));
        if (!NIL_P(v)) insecure = RTEST(v) ? 1 : 0;
    }

    CURL *h = get_thread_curl();
    if (!h) rb_raise(rb_eRuntimeError, "curl_easy_init failed");

    fetch_ctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.handle = h;

    curl_easy_setopt(h, CURLOPT_URL, RSTRING_PTR(url_v));
    /* HTTP/2 over TLS when available, with graceful downgrade to 1.1
     * on legacy servers. CURL_HTTP_VERSION_2TLS lets curl decide via
     * ALPN — non-HTTPS targets fall back to HTTP/1.1 automatically. */
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    /* Accept-Encoding goes through CURLOPT_HTTPHEADER below, not
     * CURLOPT_ACCEPT_ENCODING. The latter binds decompression to
     * libcurl's compile-time codec set and aborts the response on
     * encodings curl wasn't built for — which would defeat our
     * point of shipping in-process brotli/zstd. */
    fc.req_headers = curl_slist_append(
        fc.req_headers, "Accept-Encoding: identity");
    /* Replaced just below if any codec is linked. */
    if (scrap_accept_encoding()[0] && strcmp(scrap_accept_encoding(), "identity") != 0) {
        char ae_line[160];
        snprintf(ae_line, sizeof(ae_line), "Accept-Encoding: %s",
                 scrap_accept_encoding());
        /* Pop the identity line and replace. curl_slist has no
         * direct replace, so we rebuild from scratch. */
        curl_slist_free_all(fc.req_headers);
        fc.req_headers = NULL;
        fc.req_headers = curl_slist_append(fc.req_headers, ae_line);
    }
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, (long)follow);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, max_redirs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  /* required for use inside Ruby */
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, cb_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &fc.body);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, cb_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &fc.headers);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    if (insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (!NIL_P(headers_v)) {
        VALUE keys = rb_funcall(headers_v, rb_intern("keys"), 0);
        long n = RARRAY_LEN(keys);
        for (long i = 0; i < n; i++) {
            VALUE k = rb_ary_entry(keys, i);
            VALUE v = rb_hash_aref(headers_v, k);
            VALUE line = rb_str_dup(k);
            rb_str_cat_cstr(line, ": ");
            rb_str_append(line, v);
            fc.req_headers = curl_slist_append(fc.req_headers, RSTRING_PTR(line));
        }
    }
    /* Always set the slist — at minimum it carries Accept-Encoding so
     * curl forwards our codec advertisement rather than its own
     * (which would let curl claim decompression responsibility we
     * mean to keep). */
    if (fc.req_headers) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, fc.req_headers);
    }

    /* Drop the GVL while curl is on the network. Other Ruby threads
     * (background loaders, log writers, etc.) keep moving during the
     * round-trip. */
    rb_thread_call_without_gvl(do_fetch_nogvl, &fc, NULL, NULL);

    if (fc.req_headers) curl_slist_free_all(fc.req_headers);

    if (fc.rc != CURLE_OK) {
        const char *err = curl_easy_strerror(fc.rc);
        free(fc.body.data);
        free(fc.headers.data);
        rb_raise(rb_eIOError, "scrapetor http: %s", err);
    }

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    char *eff_url = NULL;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff_url);
    long http_ver = 0;
    curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &http_ver);

    VALUE headers_h = parse_headers_blob(fc.headers.data ? fc.headers.data : "",
                                         fc.headers.len);

    /* If a Content-Encoding header is still present, libcurl couldn't
     * decode it (it strips the header on successful auto-decompress).
     * Try our in-process decoders for brotli / zstd. On success,
     * remove the header so the body matches what callers see. */
    {
        VALUE ce_key = rb_str_new_cstr("content-encoding");
        VALUE ce_val = rb_hash_lookup(headers_h, ce_key);
        if (!NIL_P(ce_val)) {
            const char *ce = RSTRING_PTR(ce_val);
            long ce_len = RSTRING_LEN(ce_val);
            /* Trim surrounding whitespace + match the bare codec name. */
            while (ce_len > 0 && (*ce == ' ' || *ce == '\t')) { ce++; ce_len--; }
            while (ce_len > 0 && (ce[ce_len-1] == ' ' || ce[ce_len-1] == '\t' ||
                                   ce[ce_len-1] == '\r' || ce[ce_len-1] == '\n')) ce_len--;
            int decoded = 0;
#ifdef HAVE_ZLIB
            if (ce_len == 4 &&
                (ce[0] == 'g' || ce[0] == 'G') &&
                (ce[1] == 'z' || ce[1] == 'Z') &&
                (ce[2] == 'i' || ce[2] == 'I') &&
                (ce[3] == 'p' || ce[3] == 'P')) {
                char *out = NULL; size_t out_len = 0;
                /* 47 = 15 + 32; +32 enables gzip+zlib auto-detect. */
                if (scrap_zlib_decode(fc.body.data, fc.body.len, 47,
                                      &out, &out_len)) {
                    free(fc.body.data);
                    fc.body.data = out; fc.body.len = out_len; fc.body.cap = out_len;
                    decoded = 1;
                }
            }
            if (!decoded && ce_len == 7 &&
                (ce[0] == 'd' || ce[0] == 'D') &&
                (ce[1] == 'e' || ce[1] == 'E') &&
                (ce[2] == 'f' || ce[2] == 'F') &&
                (ce[3] == 'l' || ce[3] == 'L') &&
                (ce[4] == 'a' || ce[4] == 'A') &&
                (ce[5] == 't' || ce[5] == 'T') &&
                (ce[6] == 'e' || ce[6] == 'E')) {
                char *out = NULL; size_t out_len = 0;
                /* Try raw deflate first (-15), fall back to zlib wrapper (15).
                 * Real-world Content-Encoding: deflate is sent both ways. */
                if (!scrap_zlib_decode(fc.body.data, fc.body.len, -15,
                                       &out, &out_len)) {
                    if (scrap_zlib_decode(fc.body.data, fc.body.len, 15,
                                          &out, &out_len)) {
                        free(fc.body.data);
                        fc.body.data = out; fc.body.len = out_len; fc.body.cap = out_len;
                        decoded = 1;
                    }
                } else {
                    free(fc.body.data);
                    fc.body.data = out; fc.body.len = out_len; fc.body.cap = out_len;
                    decoded = 1;
                }
            }
#endif
#ifdef HAVE_BROTLI
            if (!decoded && ce_len == 2 && (ce[0] == 'b' || ce[0] == 'B') &&
                                            (ce[1] == 'r' || ce[1] == 'R')) {
                char *out = NULL; size_t out_len = 0;
                if (scrap_brotli_decode(fc.body.data, fc.body.len, &out, &out_len)) {
                    free(fc.body.data);
                    fc.body.data = out; fc.body.len = out_len; fc.body.cap = out_len;
                    decoded = 1;
                }
            }
#endif
#ifdef HAVE_ZSTD
            if (!decoded && ce_len == 4 &&
                (ce[0] == 'z' || ce[0] == 'Z') &&
                (ce[1] == 's' || ce[1] == 'S') &&
                (ce[2] == 't' || ce[2] == 'T') &&
                (ce[3] == 'd' || ce[3] == 'D')) {
                char *out = NULL; size_t out_len = 0;
                if (scrap_zstd_decode(fc.body.data, fc.body.len, &out, &out_len)) {
                    free(fc.body.data);
                    fc.body.data = out; fc.body.len = out_len; fc.body.cap = out_len;
                    decoded = 1;
                }
            }
#endif
            if (decoded) {
                rb_hash_delete(headers_h, ce_key);
            }
        }
    }

    VALUE body_s = rb_str_new(fc.body.data ? fc.body.data : "", (long)fc.body.len);
    /* HTML bytes — let the user pick the encoding via parse layers.
     * Default to UTF-8 since most real-world traffic is. */
    rb_enc_associate(body_s, enc_utf8);

    free(fc.body.data);
    free(fc.headers.data);

    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("status")),       LONG2NUM(status));
    rb_hash_aset(result, ID2SYM(rb_intern("headers")),      headers_h);
    rb_hash_aset(result, ID2SYM(rb_intern("body")),         body_s);
    rb_hash_aset(result, ID2SYM(rb_intern("final_url")),
                 rb_str_new_cstr(eff_url ? eff_url : RSTRING_PTR(url_v)));
    const char *hv_str = "1.1";
    switch (http_ver) {
        case CURL_HTTP_VERSION_1_0: hv_str = "1.0"; break;
        case CURL_HTTP_VERSION_1_1: hv_str = "1.1"; break;
        case CURL_HTTP_VERSION_2_0: hv_str = "2";   break;
#ifdef CURL_HTTP_VERSION_3
        case CURL_HTTP_VERSION_3:   hv_str = "3";   break;
#endif
    }
    rb_hash_aset(result, ID2SYM(rb_intern("http_version")),
                 rb_str_new_cstr(hv_str));

    return result;
}

static VALUE scrap_http_features(VALUE self) {
    (void)self;
    VALUE h = rb_hash_new();
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    rb_hash_aset(h, ID2SYM(rb_intern("curl_version")),
                 rb_str_new_cstr(vi->version));
    rb_hash_aset(h, ID2SYM(rb_intern("http2")),
                 (vi->features & CURL_VERSION_HTTP2) ? Qtrue : Qfalse);

    /* "brotli" / "zstd" reflect what *we* can deliver, not what
     * curl can. True if either curl was built with it OR we link
     * the codec library directly (HAVE_BROTLI / HAVE_ZSTD) for
     * in-process decoding. */
    int has_brotli = 0;
#ifdef CURL_VERSION_BROTLI
    has_brotli |= (vi->features & CURL_VERSION_BROTLI) ? 1 : 0;
#endif
#ifdef HAVE_BROTLI
    has_brotli = 1;
#endif
    rb_hash_aset(h, ID2SYM(rb_intern("brotli")), has_brotli ? Qtrue : Qfalse);
#ifdef HAVE_BROTLI
    rb_hash_aset(h, ID2SYM(rb_intern("brotli_inproc")), Qtrue);
#else
    rb_hash_aset(h, ID2SYM(rb_intern("brotli_inproc")), Qfalse);
#endif

    int has_zstd = 0;
#ifdef CURL_VERSION_ZSTD
    has_zstd |= (vi->features & CURL_VERSION_ZSTD) ? 1 : 0;
#endif
#ifdef HAVE_ZSTD
    has_zstd = 1;
#endif
    rb_hash_aset(h, ID2SYM(rb_intern("zstd")), has_zstd ? Qtrue : Qfalse);
#ifdef HAVE_ZSTD
    rb_hash_aset(h, ID2SYM(rb_intern("zstd_inproc")), Qtrue);
#else
    rb_hash_aset(h, ID2SYM(rb_intern("zstd_inproc")), Qfalse);
#endif

    rb_hash_aset(h, ID2SYM(rb_intern("libz")),
                 (vi->features & CURL_VERSION_LIBZ) ? Qtrue : Qfalse);
    rb_hash_aset(h, ID2SYM(rb_intern("accept_encoding")),
                 rb_str_new_cstr(scrap_accept_encoding()));
    return h;
}

/* ---- parallel fetch ---------------------------------------------- *
 * N concurrent libcurl GETs across pthread workers. Each worker uses
 * get_thread_curl() so its handle's connection cache persists for the
 * duration of the batch — back-to-back URLs against the same host on
 * the same worker reuse the TLS + HTTP/2 session.
 *
 * The whole batch runs under one rb_thread_call_without_gvl; Ruby's
 * other threads stay live for the entire pool of fetches. Header
 * parsing into Ruby Hashes is deferred to after-join (it needs GVL),
 * but the network and the in-process decompression all happen no-GVL.
 */

typedef struct {
    char  *url;
    long   status;
    long   http_version;
    char  *body;          size_t body_len;
    char  *headers_blob;  size_t headers_len;
    char  *final_url;
    CURLcode rc;
    char   errstr[CURL_ERROR_SIZE];
    /* Per-item Accept-Encoding header — points at a shared slist for
     * the whole batch. Not owned. */
    struct curl_slist *shared_headers;
    long   timeout_ms;
    long   max_redirects;
    int    follow_redirects;
    int    insecure;
    const char *user_agent;
} pfetch_item_t;

static void pfetch_item_free(pfetch_item_t *it) {
    free(it->url);
    free(it->body);
    free(it->headers_blob);
    free(it->final_url);
}

/* Strip Content-Encoding from a header blob (in place) and run the
 * matching in-process decoder against the body buffer. Returns 1 on
 * success or no-op, 0 on decoder failure. */
static int pfetch_decode_body(pfetch_item_t *it) {
    if (!it->headers_blob) return 1;
    /* Find Content-Encoding line. Header blob is "Name: value\r\n..."
     * concatenated. We need the value of the last Content-Encoding
     * line (curl's blob includes redirected status lines too). */
    const char *ce_val = NULL; size_t ce_len = 0;
    const char *blob = it->headers_blob;
    size_t blen = it->headers_len;
    size_t i = 0;
    while (i < blen) {
        size_t ls = i;
        while (i < blen && blob[i] != '\n') i++;
        size_t le = i;
        if (le > ls && blob[le-1] == '\r') le--;
        if (i < blen) i++;
        if (le == ls) continue;
        size_t colon = (size_t)-1;
        for (size_t k = ls; k < le; k++) {
            if (blob[k] == ':') { colon = k; break; }
        }
        if (colon == (size_t)-1) continue;
        if (colon - ls != 16) continue;
        /* case-insensitive compare "content-encoding" */
        const char *want = "content-encoding";
        int matches = 1;
        for (size_t k = 0; k < 16; k++) {
            char a = blob[ls + k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != want[k]) { matches = 0; break; }
        }
        if (!matches) continue;
        size_t vs = colon + 1;
        while (vs < le && (blob[vs] == ' ' || blob[vs] == '\t')) vs++;
        ce_val = blob + vs; ce_len = le - vs;
    }
    if (!ce_val) return 1;
    while (ce_len > 0 && (ce_val[ce_len-1] == ' ' || ce_val[ce_len-1] == '\t' ||
                          ce_val[ce_len-1] == '\r' || ce_val[ce_len-1] == '\n')) ce_len--;

    char *out = NULL; size_t out_len = 0;
    int decoded = 0;
#ifdef HAVE_ZLIB
    if (ce_len == 4 && ((ce_val[0] | 0x20) == 'g') && ((ce_val[1] | 0x20) == 'z') &&
        ((ce_val[2] | 0x20) == 'i') && ((ce_val[3] | 0x20) == 'p')) {
        decoded = scrap_zlib_decode(it->body, it->body_len, 47, &out, &out_len);
    } else if (ce_len == 7 && ((ce_val[0] | 0x20) == 'd') && ((ce_val[1] | 0x20) == 'e') &&
               ((ce_val[2] | 0x20) == 'f') && ((ce_val[3] | 0x20) == 'l') &&
               ((ce_val[4] | 0x20) == 'a') && ((ce_val[5] | 0x20) == 't') &&
               ((ce_val[6] | 0x20) == 'e')) {
        if (!scrap_zlib_decode(it->body, it->body_len, -15, &out, &out_len)) {
            decoded = scrap_zlib_decode(it->body, it->body_len, 15, &out, &out_len);
        } else decoded = 1;
    }
#endif
#ifdef HAVE_BROTLI
    if (!decoded && ce_len == 2 && ((ce_val[0] | 0x20) == 'b') && ((ce_val[1] | 0x20) == 'r')) {
        decoded = scrap_brotli_decode(it->body, it->body_len, &out, &out_len);
    }
#endif
#ifdef HAVE_ZSTD
    if (!decoded && ce_len == 4 && ((ce_val[0] | 0x20) == 'z') && ((ce_val[1] | 0x20) == 's') &&
        ((ce_val[2] | 0x20) == 't') && ((ce_val[3] | 0x20) == 'd')) {
        decoded = scrap_zstd_decode(it->body, it->body_len, &out, &out_len);
    }
#endif
    if (decoded) {
        free(it->body);
        it->body = out; it->body_len = out_len;
    }
    return 1;
}

static void pfetch_do_one(pfetch_item_t *it) {
    CURL *h = get_thread_curl();
    if (!h) { it->rc = CURLE_FAILED_INIT; return; }

    buf_t bbuf; memset(&bbuf, 0, sizeof(bbuf));
    buf_t hbuf; memset(&hbuf, 0, sizeof(hbuf));

    curl_easy_setopt(h, CURLOPT_URL, it->url);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(h, CURLOPT_USERAGENT, it->user_agent);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, (long)it->follow_redirects);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, it->max_redirects);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, it->timeout_ms);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, cb_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &bbuf);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, cb_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &hbuf);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, it->errstr);
    if (it->insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (it->shared_headers) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, it->shared_headers);
    }

    it->rc = curl_easy_perform(h);

    if (it->rc == CURLE_OK) {
        long status = 0, hv = 0;
        char *eff = NULL;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &hv);
        curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
        it->status = status;
        it->http_version = hv;
        if (eff) {
            size_t l = strlen(eff);
            it->final_url = (char *)malloc(l + 1);
            memcpy(it->final_url, eff, l + 1);
        }
    }
    it->body = bbuf.data; it->body_len = bbuf.len;
    it->headers_blob = hbuf.data; it->headers_len = hbuf.len;

    /* In-process decompression while we still hold no GVL. */
    if (it->rc == CURLE_OK) pfetch_decode_body(it);
}

typedef struct {
    pfetch_item_t *items;
    size_t         n;
    int            next_idx;
} pfetch_ctx_t;

static void *pfetch_worker(void *arg) {
    pfetch_ctx_t *ctx = (pfetch_ctx_t *)arg;
    while (1) {
        int i = __atomic_fetch_add(&ctx->next_idx, 1, __ATOMIC_RELAXED);
        if (i >= (int)ctx->n) return NULL;
        pfetch_do_one(&ctx->items[i]);
    }
}

typedef struct {
    pfetch_ctx_t *ctx;
    int           n_threads;
} pfetch_run_arg_t;

static void *pfetch_run(void *arg) {
    pfetch_run_arg_t *ra = (pfetch_run_arg_t *)arg;
    int nt = ra->n_threads;
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)nt);
    int spawned = 0;
    for (int i = 0; i < nt; i++) {
        if (pthread_create(&threads[i], NULL, pfetch_worker, ra->ctx) == 0) spawned++;
    }
    if (spawned < nt) pfetch_worker(ra->ctx);
    for (int i = 0; i < spawned; i++) pthread_join(threads[i], NULL);
    free(threads);
    return NULL;
}

static VALUE scrap_parallel_fetch(int argc, VALUE *argv, VALUE self) {
    (void)self;
    VALUE urls_v, opts_v;
    rb_scan_args(argc, argv, "11", &urls_v, &opts_v);
    Check_Type(urls_v, T_ARRAY);
    long n = RARRAY_LEN(urls_v);
    if (n == 0) return rb_ary_new();

    int n_threads = 4;
    long timeout_ms = 30000;
    int  follow = 1;
    long max_redirs = 10;
    const char *ua = "scrapetor/0.1 (libcurl)";
    int  insecure = 0;
    VALUE headers_v = Qnil;
    if (!NIL_P(opts_v)) {
        Check_Type(opts_v, T_HASH);
        VALUE v;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("threads")));
        if (!NIL_P(v)) n_threads = NUM2INT(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("timeout_ms")));
        if (!NIL_P(v)) timeout_ms = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("follow_redirects")));
        if (!NIL_P(v)) follow = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("max_redirects")));
        if (!NIL_P(v)) max_redirs = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("user_agent")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); ua = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("insecure")));
        if (!NIL_P(v)) insecure = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("headers")));
        if (!NIL_P(v)) { Check_Type(v, T_HASH); headers_v = v; }
    }
    if (n_threads < 1) n_threads = 1;
    if (n_threads > (int)n) n_threads = (int)n;

    /* One shared slist for the whole batch: Accept-Encoding + user
     * headers. All workers point at this; no mutation after build. */
    struct curl_slist *shared = NULL;
    {
        char ae_line[160];
        snprintf(ae_line, sizeof(ae_line), "Accept-Encoding: %s", scrap_accept_encoding());
        shared = curl_slist_append(shared, ae_line);
    }
    if (!NIL_P(headers_v)) {
        VALUE keys = rb_funcall(headers_v, rb_intern("keys"), 0);
        long nk = RARRAY_LEN(keys);
        for (long i = 0; i < nk; i++) {
            VALUE k = rb_ary_entry(keys, i);
            VALUE vv = rb_hash_aref(headers_v, k);
            VALUE line = rb_str_dup(k);
            rb_str_cat_cstr(line, ": ");
            rb_str_append(line, vv);
            shared = curl_slist_append(shared, RSTRING_PTR(line));
        }
    }

    pfetch_item_t *items = (pfetch_item_t *)calloc((size_t)n, sizeof(pfetch_item_t));
    for (long i = 0; i < n; i++) {
        VALUE u = rb_ary_entry(urls_v, i);
        Check_Type(u, T_STRING);
        size_t ul = (size_t)RSTRING_LEN(u);
        items[i].url = (char *)malloc(ul + 1);
        memcpy(items[i].url, RSTRING_PTR(u), ul);
        items[i].url[ul] = 0;
        items[i].shared_headers = shared;
        items[i].timeout_ms = timeout_ms;
        items[i].follow_redirects = follow;
        items[i].max_redirects = max_redirs;
        items[i].user_agent = ua;
        items[i].insecure = insecure;
    }

    pfetch_ctx_t ctx; ctx.items = items; ctx.n = (size_t)n; ctx.next_idx = 0;
    pfetch_run_arg_t ra; ra.ctx = &ctx; ra.n_threads = n_threads;
    rb_thread_call_without_gvl(pfetch_run, &ra, NULL, NULL);

    /* Re-acquired GVL — assemble Ruby Hashes from the C results. */
    VALUE result = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        pfetch_item_t *it = &items[i];
        VALUE h = rb_hash_new();
        if (it->rc != CURLE_OK) {
            VALUE err = rb_hash_new();
            rb_hash_aset(err, ID2SYM(rb_intern("url")), rb_str_new_cstr(it->url));
            rb_hash_aset(err, ID2SYM(rb_intern("error")),
                         rb_str_new_cstr(it->errstr[0] ? it->errstr : curl_easy_strerror(it->rc)));
            rb_hash_aset(h, ID2SYM(rb_intern("error")), err);
            rb_ary_push(result, h);
            pfetch_item_free(it);
            continue;
        }
        rb_hash_aset(h, ID2SYM(rb_intern("status")), LONG2NUM(it->status));
        rb_hash_aset(h, ID2SYM(rb_intern("body")),
                     rb_enc_str_new(it->body ? it->body : "", (long)it->body_len, enc_utf8));
        VALUE headers_h = parse_headers_blob(it->headers_blob ? it->headers_blob : "",
                                             it->headers_len);
        /* Drop CE so headers + body stay consistent. */
        rb_hash_delete(headers_h, rb_str_new_cstr("content-encoding"));
        rb_hash_aset(h, ID2SYM(rb_intern("headers")), headers_h);
        rb_hash_aset(h, ID2SYM(rb_intern("final_url")),
                     rb_str_new_cstr(it->final_url ? it->final_url : it->url));
        const char *hv_str = "1.1";
        switch (it->http_version) {
            case CURL_HTTP_VERSION_1_0: hv_str = "1.0"; break;
            case CURL_HTTP_VERSION_1_1: hv_str = "1.1"; break;
            case CURL_HTTP_VERSION_2_0: hv_str = "2";   break;
#ifdef CURL_HTTP_VERSION_3
            case CURL_HTTP_VERSION_3:   hv_str = "3";   break;
#endif
        }
        rb_hash_aset(h, ID2SYM(rb_intern("http_version")), rb_str_new_cstr(hv_str));
        rb_ary_push(result, h);
        pfetch_item_free(it);
    }
    free(items);
    curl_slist_free_all(shared);
    return result;
}

void Init_scrapetor_http(VALUE mod_native) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    VALUE mod_http = rb_define_module_under(mod_native, "Http");
    rb_define_singleton_method(mod_http, "get",            scrap_http_get,         -1);
    rb_define_singleton_method(mod_http, "parallel_fetch", scrap_parallel_fetch,   -1);
    rb_define_singleton_method(mod_http, "features",       scrap_http_features,     0);
    rb_define_const(mod_http, "AVAILABLE", Qtrue);
}

#else  /* HAVE_LIBCURL */

/* Stub: HTTP layer was not built (libcurl missing at compile time).
 * Define the constant so the Ruby side can detect this and provide a
 * useful error. */
void Init_scrapetor_http(VALUE mod_native) {
    VALUE mod_http = rb_define_module_under(mod_native, "Http");
    rb_define_const(mod_http, "AVAILABLE", Qfalse);
}

#endif
