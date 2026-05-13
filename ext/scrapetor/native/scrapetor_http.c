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
#include <time.h>
#include <errno.h>
#include <iconv.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Hooks into scrapetor_dom.c so we can run dom_parse on each
 * response body inside the same no-GVL worker that fetched it. */
typedef struct dom_doc dom_doc_t;
extern dom_doc_t *scrap_dom_make_owned_doc(char *bytes, size_t len);
extern void       scrap_dom_parse_eager_nocache(dom_doc_t *d);
extern VALUE      scrap_dom_wrap_doc(VALUE klass, dom_doc_t *d);

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

/* ---- per-host throttle table ------------------------------------- *
 * Global, thread-safe map from host name to last-request timestamp.
 * Drives polite-scraping rate limits across both single-fetch and
 * parallel-fetch paths — a parallel batch of 32 URLs against the same
 * host with rate_limit_ms=500 will serialise at that host through this
 * table even though the worker threads themselves are independent.
 *
 * 256 slots is plenty: scrapers target dozens of hosts max in practice;
 * past that we round-robin LRU evictions.
 */
typedef struct {
    char     *host;        /* malloc'd, lowercase */
    uint64_t  last_ns;     /* monotonic-ns timestamp of last completed wait */
    uint32_t  hits;        /* LRU counter for eviction */
} throttle_slot_t;

#define THROTTLE_CAP 256
static throttle_slot_t g_throttle[THROTTLE_CAP];
static int             g_throttle_n = 0;
static pthread_mutex_t g_throttle_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Pull the host out of a URL: bytes between "://" and the next
 * '/', '?', '#', or ':'. Returns 1 on success. */
static int scrap_extract_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    const char *e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#' && *e != ':') e++;
    size_t l = (size_t)(e - p);
    if (l == 0 || l + 1 > cap) return 0;
    for (size_t i = 0; i < l; i++) {
        char c = p[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[l] = 0;
    return 1;
}

/* Wait long enough since the last request to `host` to honour the
 * min interval, then mark the new "last" time. The sleep happens
 * outside the mutex so concurrent workers for different hosts don't
 * block each other.
 *
 * Safe to call from a no-GVL worker — uses nanosleep, no Ruby state. */
static void scrap_throttle_wait(const char *host, uint64_t min_interval_ns) {
    if (!host || !*host || min_interval_ns == 0) return;
    pthread_mutex_lock(&g_throttle_mu);
    int idx = -1;
    for (int i = 0; i < g_throttle_n; i++) {
        if (strcmp(g_throttle[i].host, host) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_throttle_n < THROTTLE_CAP) {
            idx = g_throttle_n++;
        } else {
            /* Evict the least-recently-used slot. */
            int lru = 0;
            for (int i = 1; i < THROTTLE_CAP; i++) {
                if (g_throttle[i].hits < g_throttle[lru].hits) lru = i;
            }
            idx = lru;
            free(g_throttle[idx].host);
            g_throttle[idx].host = NULL;
        }
        g_throttle[idx].host = strdup(host);
        g_throttle[idx].last_ns = 0;
        g_throttle[idx].hits = 0;
    }
    g_throttle[idx].hits++;
    uint64_t now = mono_ns();
    /* last_ns is the "earliest allowed start" for the next request to
     * this host. Each worker reserves its slot by advancing
     * last_ns = max(now, last_ns) + min_interval_ns. Concurrent workers
     * to the same host see ever-increasing reservations and serialise
     * cleanly; concurrent workers to *different* hosts hit different
     * slots and don't block each other. */
    uint64_t earliest = g_throttle[idx].last_ns;
    uint64_t start = (earliest <= now) ? now : earliest;
    uint64_t wait_ns = start - now;
    g_throttle[idx].last_ns = start + min_interval_ns;
    pthread_mutex_unlock(&g_throttle_mu);

    if (wait_ns > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)(wait_ns / 1000000000ull);
        ts.tv_nsec = (long)(wait_ns % 1000000000ull);
        nanosleep(&ts, NULL);
    }
}

/* ---- HTTP response cache (ETag / Last-Modified) ------------------ *
 * Disk-backed cache of completed GET responses keyed by URL. Each entry
 * stores status + ETag + Last-Modified + Content-Type + body in a
 * tagged binary format:
 *
 *   8 bytes  magic "SCRHV001"
 *   4 bytes  uint32_le status
 *   4 + N    etag       (length-prefixed)
 *   4 + N    lastmod    (length-prefixed)
 *   4 + N    ctype      (length-prefixed)
 *   8 + N    body       (uint64_le length-prefixed)
 *
 * When :cache_dir is set on a request and a cache entry exists for the
 * URL, the fetch adds If-None-Match / If-Modified-Since automatically.
 * A 304 response is served from the cached body with the cached
 * Content-Type — the network round-trip stayed cheap (no body) but
 * the caller sees a fully-formed 200-shaped response.
 *
 * Cache key is a 128-bit FNV1a-double (two FNV64s with different
 * seeds) — collision probability for any realistic crawl is
 * effectively zero, and avoiding a SHA-2 dep keeps the binary lean.
 */

static void scrap_cache_key(const char *url, char out[33]) {
    uint64_t h1 = 0xcbf29ce484222325ull;
    uint64_t h2 = 0x84222325cbf29ce4ull;
    for (size_t i = 0; url[i]; i++) {
        uint8_t c = (uint8_t)url[i];
        h1 ^= c; h1 *= 0x100000001b3ull;
        h2 ^= c; h2 *= 0x9e3779b97f4a7c15ull;
    }
    snprintf(out, 33, "%016llx%016llx",
             (unsigned long long)h1, (unsigned long long)h2);
}

typedef struct {
    long  status;
    char *etag;       size_t etag_len;
    char *lastmod;    size_t lastmod_len;
    char *ctype;      size_t ctype_len;
    char *body;       size_t body_len;
} scrap_cache_entry_t;

static void scrap_cache_entry_free(scrap_cache_entry_t *e) {
    free(e->etag); free(e->lastmod); free(e->ctype); free(e->body);
    memset(e, 0, sizeof(*e));
}

static int scrap_cache_path(const char *dir, const char *url,
                            char *out, size_t cap) {
    char key[33];
    scrap_cache_key(url, key);
    int n = snprintf(out, cap, "%s/%c%c", dir, key[0], key[1]);
    if (n <= 0 || (size_t)n >= cap) return 0;
    mkdir(dir, 0755);  /* best-effort; the leaf mkdir below is what matters */
    mkdir(out, 0755);
    return snprintf(out, cap, "%s/%c%c/%s.cache", dir, key[0], key[1], key) > 0;
}

static int read_u32_le(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}
static int read_u64_le(FILE *f, uint64_t *out) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    *out = 0;
    for (int i = 0; i < 8; i++) *out |= (uint64_t)b[i] << (i * 8);
    return 1;
}
static int read_lenstr(FILE *f, char **out, size_t *out_len) {
    uint32_t l;
    if (!read_u32_le(f, &l)) return 0;
    *out_len = l;
    if (l == 0) { *out = NULL; return 1; }
    *out = (char *)malloc(l + 1);
    if (fread(*out, 1, l, f) != l) { free(*out); *out = NULL; return 0; }
    (*out)[l] = 0;
    return 1;
}

static int scrap_cache_load(const char *dir, const char *url,
                            scrap_cache_entry_t *e) {
    memset(e, 0, sizeof(*e));
    char path[1024];
    if (!scrap_cache_path(dir, url, path, sizeof(path))) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SCRHV001", 8) != 0) {
        fclose(f); return 0;
    }
    uint32_t status;
    if (!read_u32_le(f, &status)) { fclose(f); return 0; }
    e->status = (long)status;
    if (!read_lenstr(f, &e->etag, &e->etag_len)) { fclose(f); return 0; }
    if (!read_lenstr(f, &e->lastmod, &e->lastmod_len)) { fclose(f); return 0; }
    if (!read_lenstr(f, &e->ctype, &e->ctype_len)) { fclose(f); return 0; }
    uint64_t body_len;
    if (!read_u64_le(f, &body_len)) { fclose(f); return 0; }
    e->body_len = body_len;
    if (body_len > 0) {
        e->body = (char *)malloc(body_len + 1);
        if (fread(e->body, 1, body_len, f) != body_len) {
            fclose(f); scrap_cache_entry_free(e); return 0;
        }
        e->body[body_len] = 0;
    }
    fclose(f);
    return 1;
}

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    fwrite(b, 1, 4, f);
}
static void write_u64_le(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (v >> (i * 8)) & 0xFF;
    fwrite(b, 1, 8, f);
}
static void write_lenstr(FILE *f, const char *p, size_t l) {
    write_u32_le(f, (uint32_t)l);
    if (l) fwrite(p, 1, l, f);
}

static int scrap_cache_store(const char *dir, const char *url,
                             long status, const char *etag, size_t etag_len,
                             const char *lastmod, size_t lastmod_len,
                             const char *ctype, size_t ctype_len,
                             const char *body, size_t body_len) {
    char path[1024];
    if (!scrap_cache_path(dir, url, path, sizeof(path))) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fwrite("SCRHV001", 1, 8, f);
    write_u32_le(f, (uint32_t)status);
    write_lenstr(f, etag,    etag_len);
    write_lenstr(f, lastmod, lastmod_len);
    write_lenstr(f, ctype,   ctype_len);
    write_u64_le(f, (uint64_t)body_len);
    if (body_len) fwrite(body, 1, body_len, f);
    fclose(f);
    return rename(tmp, path) == 0;
}

/* ---- charset detection + iconv transcode to UTF-8 ----------------- *
 * Find charset=... in a Content-Type header value (length-bounded), then
 * iconv-transcode the body buffer in place. Replacement bytes are used
 * for invalid sequences so the parse layer never trips on undecodable
 * input. UTF-8 / utf8 / absent charset all skip the conversion.
 */
static int scrap_extract_charset(const char *ct, size_t ct_len,
                                 char *out, size_t cap) {
    const char *needle = "charset";
    size_t needle_len = 7;
    for (size_t i = 0; i + needle_len < ct_len; i++) {
        int ok = 1;
        for (size_t j = 0; j < needle_len; j++) {
            char a = ct[i + j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != needle[j]) { ok = 0; break; }
        }
        if (!ok) continue;
        size_t j = i + needle_len;
        while (j < ct_len && (ct[j] == ' ' || ct[j] == '\t')) j++;
        if (j >= ct_len || ct[j] != '=') continue;
        j++;
        while (j < ct_len && (ct[j] == ' ' || ct[j] == '\t')) j++;
        char quote = 0;
        if (j < ct_len && (ct[j] == '"' || ct[j] == '\'')) { quote = ct[j]; j++; }
        size_t s = j;
        while (j < ct_len &&
               (quote ? (ct[j] != quote)
                      : (ct[j] != ';' && ct[j] != ' ' && ct[j] != '\t' &&
                         ct[j] != '\r' && ct[j] != '\n'))) j++;
        size_t l = j - s;
        if (l == 0 || l + 1 > cap) return 0;
        memcpy(out, ct + s, l);
        out[l] = 0;
        return 1;
    }
    return 0;
}

static int scrap_transcode_to_utf8(char **body, size_t *body_len, size_t *body_cap,
                                   const char *charset) {
    if (!charset || !*charset) return 0;
    if (strcasecmp(charset, "utf-8") == 0 ||
        strcasecmp(charset, "utf8")  == 0 ||
        strcasecmp(charset, "us-ascii") == 0 ||
        strcasecmp(charset, "ascii") == 0) return 0;
    iconv_t cd = iconv_open("UTF-8", charset);
    if (cd == (iconv_t)-1) return 0;

    size_t in_left = *body_len;
    char  *in_ptr  = *body;
    size_t out_cap = (*body_len) * 2 + 16;
    char  *out     = (char *)malloc(out_cap);
    char  *out_ptr = out;
    size_t out_left = out_cap;

    while (in_left > 0) {
        size_t r = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (r != (size_t)-1) continue;
        if (errno == EILSEQ || errno == EINVAL) {
            /* Replace the offending byte with '?' and skip it. */
            if (out_left < 1) {
                size_t used = (size_t)(out_ptr - out);
                out_cap *= 2;
                out = (char *)realloc(out, out_cap);
                out_ptr = out + used;
                out_left = out_cap - used;
            }
            *out_ptr++ = '?'; out_left--;
            in_ptr++; in_left--;
        } else if (errno == E2BIG) {
            size_t used = (size_t)(out_ptr - out);
            out_cap *= 2;
            out = (char *)realloc(out, out_cap);
            out_ptr = out + used;
            out_left = out_cap - used;
        } else {
            iconv_close(cd);
            free(out);
            return 0;
        }
    }
    iconv_close(cd);
    free(*body);
    *body = out;
    *body_len = (size_t)(out_ptr - out);
    *body_cap = out_cap;
    return 1;
}

/* Pull Content-Type out of a header blob and run transcode if its
 * charset is non-UTF-8. No-op when there's no header, no charset, or
 * the charset is already UTF-8 / ASCII. */
static int scrap_apply_charset(const char *headers_blob, size_t headers_len,
                               char **body, size_t *body_len, size_t *body_cap) {
    const char *ct_val = NULL; size_t ct_vlen = 0;
    size_t i = 0;
    while (i < headers_len) {
        size_t ls = i;
        while (i < headers_len && headers_blob[i] != '\n') i++;
        size_t le = i;
        if (le > ls && headers_blob[le-1] == '\r') le--;
        if (i < headers_len) i++;
        if (le == ls) continue;
        size_t colon = (size_t)-1;
        for (size_t k = ls; k < le; k++) {
            if (headers_blob[k] == ':') { colon = k; break; }
        }
        if (colon == (size_t)-1) continue;
        if (colon - ls != 12) continue;
        const char *want = "content-type";
        int ok = 1;
        for (size_t k = 0; k < 12; k++) {
            char a = headers_blob[ls + k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != want[k]) { ok = 0; break; }
        }
        if (!ok) continue;
        size_t vs = colon + 1;
        while (vs < le && (headers_blob[vs] == ' ' || headers_blob[vs] == '\t')) vs++;
        ct_val = headers_blob + vs; ct_vlen = le - vs;
    }
    if (!ct_val) return 0;
    char cs[64];
    if (!scrap_extract_charset(ct_val, ct_vlen, cs, sizeof(cs))) return 0;
    return scrap_transcode_to_utf8(body, body_len, body_cap, cs);
}

/* ---- shared connection cache (CURLSH) ---------------------------- *
 * libcurl easy handles each carry a private connection cache by
 * default. With per-thread handles that means N pthread workers
 * hitting one host open N independent TLS connections. CURLSH lets
 * them all share one connection pool, one DNS cache, and one TLS
 * session cache — so 8 workers against the same HTTP/2 origin
 * settle on one (or a few) multiplexed connections instead of
 * eight handshakes.
 *
 * libcurl requires user-provided locks for the share since the
 * shared data structures can be touched concurrently. We use one
 * pthread mutex per shared resource class. */

static CURLSH *g_share = NULL;
/* One mutex per curl_lock_data class. curl_lock_data values run from
 * 0 (NONE) up to CURL_LOCK_DATA_LAST; sizing the array to 16 covers
 * present + future entries comfortably without an unbounded VLA. */
#define SCRAP_SHARE_LOCKS 16
static pthread_mutex_t g_share_locks[SCRAP_SHARE_LOCKS];

static void scrap_share_lock(CURL *h, curl_lock_data data,
                             curl_lock_access access, void *user) {
    (void)h; (void)access; (void)user;
    if ((int)data >= 0 && (int)data < SCRAP_SHARE_LOCKS) {
        pthread_mutex_lock(&g_share_locks[(int)data]);
    }
}
static void scrap_share_unlock(CURL *h, curl_lock_data data, void *user) {
    (void)h; (void)user;
    if ((int)data >= 0 && (int)data < SCRAP_SHARE_LOCKS) {
        pthread_mutex_unlock(&g_share_locks[(int)data]);
    }
}
static void scrap_share_init(void) {
    if (g_share) return;
    for (int i = 0; i < SCRAP_SHARE_LOCKS; i++) {
        pthread_mutex_init(&g_share_locks[i], NULL);
    }
    g_share = curl_share_init();
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   scrap_share_lock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, scrap_share_unlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}

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
    /* Attach the global share so this handle pulls connections, DNS
     * results, and TLS sessions from the shared pool. Must be set
     * after every reset because curl_easy_reset clears it. */
    if (g_share) curl_easy_setopt(h, CURLOPT_SHARE, g_share);
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
    const char *method   = NULL;     /* NULL = GET */
    const char *body     = NULL;
    long  body_len       = 0;
    int   nobody         = 0;        /* HEAD */
    const char *cookiejar  = NULL;
    const char *cookiefile = NULL;
    const char *proxy      = NULL;
    const char *basic_auth = NULL;
    const char *bearer     = NULL;
    const char *ca_path    = NULL;
    long rate_limit_ms     = 0;
    int  transcode_utf8    = 1;
    const char *cache_dir  = NULL;
    VALUE multipart_v      = Qnil;

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

        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("method")));
        if (!NIL_P(v)) {
            if (SYMBOL_P(v)) v = rb_sym2str(v);
            Check_Type(v, T_STRING);
            method = RSTRING_PTR(v);
            if (strcasecmp(method, "head") == 0) { nobody = 1; method = NULL; }
            else if (strcasecmp(method, "get") == 0) method = NULL;
            else {
                /* HTTP methods are case-sensitive; uppercase so
                 * picky servers (RFC 7231 strict) accept them. */
                static char method_buf[24];
                size_t mi = 0;
                for (; mi < sizeof(method_buf) - 1 && method[mi]; mi++) {
                    char c = method[mi];
                    method_buf[mi] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
                }
                method_buf[mi] = 0;
                method = method_buf;
            }
        }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("body")));
        if (!NIL_P(v)) {
            Check_Type(v, T_STRING);
            body = RSTRING_PTR(v);
            body_len = RSTRING_LEN(v);
        }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("cookiejar")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); cookiejar = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("cookiefile")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); cookiefile = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("proxy")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); proxy = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("basic_auth")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); basic_auth = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("bearer_token")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); bearer = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("ca_path")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); ca_path = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("rate_limit_ms")));
        if (!NIL_P(v)) rate_limit_ms = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("transcode_utf8")));
        if (!NIL_P(v)) transcode_utf8 = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("cache_dir")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); cache_dir = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("multipart")));
        if (!NIL_P(v)) { Check_Type(v, T_HASH); multipart_v = v; }
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
    /* Tell curl to wait briefly for an existing HTTP/2 connection to
     * the target to become available rather than opening a fresh
     * TCP+TLS handshake. Combined with the shared CONNECT pool this
     * lets N workers multiplex through one connection per host. */
#ifdef CURLOPT_PIPEWAIT
    curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
#endif
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
    if (ca_path) {
        curl_easy_setopt(h, CURLOPT_CAINFO, ca_path);
    }

    /* Method + body. CUSTOMREQUEST overrides the verb regardless of
     * POSTFIELDS presence; libcurl auto-switches to POST when POSTFIELDS
     * is set, so we force CUSTOMREQUEST for everything non-GET to be
     * explicit. NOBODY for HEAD strips the response body. */
    if (nobody) {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD");
    } else if (method) {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    }
    if (body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }

    /* Multipart form upload. Each Hash entry becomes a form part:
     *   "field" => "string"                                  - text field
     *   "field" => { path: "...", filename: ..., content_type: ... }
     *   "field" => { data: "...bytes...", filename: ..., content_type: ... }
     * Mixed in any combination. */
    curl_mime *mime = NULL;
    if (!NIL_P(multipart_v)) {
        mime = curl_mime_init(h);
        VALUE keys = rb_funcall(multipart_v, rb_intern("keys"), 0);
        long nk = RARRAY_LEN(keys);
        for (long i = 0; i < nk; i++) {
            VALUE k = rb_ary_entry(keys, i);
            VALUE pv = rb_hash_aref(multipart_v, k);
            VALUE k_s = rb_obj_as_string(k);
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_name(part, RSTRING_PTR(k_s));
            if (RB_TYPE_P(pv, T_STRING)) {
                curl_mime_data(part, RSTRING_PTR(pv), (size_t)RSTRING_LEN(pv));
            } else if (RB_TYPE_P(pv, T_HASH)) {
                VALUE data_v     = rb_hash_aref(pv, ID2SYM(rb_intern("data")));
                VALUE path_v     = rb_hash_aref(pv, ID2SYM(rb_intern("path")));
                VALUE filename_v = rb_hash_aref(pv, ID2SYM(rb_intern("filename")));
                VALUE ctype_v    = rb_hash_aref(pv, ID2SYM(rb_intern("content_type")));
                if (!NIL_P(path_v)) {
                    Check_Type(path_v, T_STRING);
                    curl_mime_filedata(part, RSTRING_PTR(path_v));
                } else if (!NIL_P(data_v)) {
                    Check_Type(data_v, T_STRING);
                    curl_mime_data(part, RSTRING_PTR(data_v), (size_t)RSTRING_LEN(data_v));
                }
                if (!NIL_P(filename_v)) {
                    Check_Type(filename_v, T_STRING);
                    curl_mime_filename(part, RSTRING_PTR(filename_v));
                }
                if (!NIL_P(ctype_v)) {
                    Check_Type(ctype_v, T_STRING);
                    curl_mime_type(part, RSTRING_PTR(ctype_v));
                }
            } else {
                rb_raise(rb_eArgError,
                         "multipart values must be String or Hash with :path/:data");
            }
        }
        curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
    }

    if (cookiefile) curl_easy_setopt(h, CURLOPT_COOKIEFILE, cookiefile);
    if (cookiejar)  curl_easy_setopt(h, CURLOPT_COOKIEJAR,  cookiejar);
    if (!cookiefile && cookiejar) {
        /* Tell curl to start with an empty in-memory jar (so writes
         * land somewhere) even when no input file is provided. */
        curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");
    }
    if (proxy) curl_easy_setopt(h, CURLOPT_PROXY, proxy);
    if (basic_auth) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH,  (long)CURLAUTH_BASIC);
        curl_easy_setopt(h, CURLOPT_USERPWD,   basic_auth);
    }
    if (bearer) {
#ifdef CURLAUTH_BEARER
        curl_easy_setopt(h, CURLOPT_HTTPAUTH,        (long)CURLAUTH_BEARER);
        curl_easy_setopt(h, CURLOPT_XOAUTH2_BEARER,  bearer);
#else
        /* Older libcurl — fall back to a manual Authorization header. */
        char line[1024];
        snprintf(line, sizeof(line), "Authorization: Bearer %s", bearer);
        fc.req_headers = curl_slist_append(fc.req_headers, line);
#endif
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

    /* HTTP response cache lookup + revalidation. If a cache entry
     * exists for this URL, attach If-None-Match / If-Modified-Since
     * so the server can answer 304 (no body) when nothing changed. */
    scrap_cache_entry_t cached;
    memset(&cached, 0, sizeof(cached));
    int have_cached = 0;
    if (cache_dir && !nobody && !method && !body) {
        /* Cache only safe-GETs. POST/PUT/DELETE responses aren't
         * eligible per RFC 7234, and HEAD has no body to serve. */
        have_cached = scrap_cache_load(cache_dir, RSTRING_PTR(url_v), &cached);
        if (have_cached) {
            if (cached.etag_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-None-Match: %.*s",
                         (int)cached.etag_len, cached.etag);
                fc.req_headers = curl_slist_append(fc.req_headers, line);
            }
            if (cached.lastmod_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-Modified-Since: %.*s",
                         (int)cached.lastmod_len, cached.lastmod);
                fc.req_headers = curl_slist_append(fc.req_headers, line);
            }
        }
    }

    /* Per-host throttle. Honours rate_limit_ms before we even open
     * the socket; safe to call under GVL or no-GVL since it uses
     * only pthread + nanosleep. */
    if (rate_limit_ms > 0) {
        char host[256];
        if (scrap_extract_host(RSTRING_PTR(url_v), host, sizeof(host))) {
            scrap_throttle_wait(host, (uint64_t)rate_limit_ms * 1000000ull);
        }
    }

    /* Drop the GVL while curl is on the network. Other Ruby threads
     * (background loaders, log writers, etc.) keep moving during the
     * round-trip. */
    rb_thread_call_without_gvl(do_fetch_nogvl, &fc, NULL, NULL);

    if (fc.req_headers) curl_slist_free_all(fc.req_headers);
    if (mime) curl_mime_free(mime);

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

    /* Flush the cookie jar to disk now rather than at handle cleanup
     * (which happens on thread exit). Lets callers see Set-Cookie
     * values immediately after the request completes. */
    if (cookiejar) curl_easy_setopt(h, CURLOPT_COOKIELIST, "FLUSH");

    /* HTTP cache revalidation: 304 -> serve from cache; 200 with
     * ETag/Last-Modified -> store new entry. */
    int served_from_cache = 0;
    if (cache_dir && have_cached && status == 304) {
        /* Replace body buffer with cached payload; bump status to 200
         * so the caller sees a fully-formed response. The actual 304
         * round-trip was cheap (no body) — this is the cache win. */
        free(fc.body.data);
        fc.body.data = (char *)malloc(cached.body_len + 1);
        memcpy(fc.body.data, cached.body, cached.body_len);
        fc.body.data[cached.body_len] = 0;
        fc.body.len = cached.body_len;
        fc.body.cap = cached.body_len;
        status = 200;
        served_from_cache = 1;
    }

    VALUE headers_h = parse_headers_blob(fc.headers.data ? fc.headers.data : "",
                                         fc.headers.len);

    /* When we served from cache, the network response was 304 with no
     * headers other than status/ETag. Overlay the cached
     * Content-Type so consumers see the right metadata for the
     * body they're getting. */
    if (served_from_cache && cached.ctype_len > 0) {
        rb_hash_aset(headers_h, rb_str_new_cstr("content-type"),
                     rb_str_new(cached.ctype, (long)cached.ctype_len));
        rb_hash_aset(headers_h, rb_str_new_cstr("x-scrapetor-cache"),
                     rb_str_new_cstr("hit"));
    } else if (cache_dir && have_cached) {
        rb_hash_aset(headers_h, rb_str_new_cstr("x-scrapetor-cache"),
                     rb_str_new_cstr("miss-revalidated"));
    }

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

    /* Charset transcode to UTF-8. Runs after content-encoding decode
     * so iconv sees the raw decoded text. */
    if (transcode_utf8 && fc.body.data && fc.body.len > 0) {
        if (scrap_apply_charset(fc.headers.data ? fc.headers.data : "", fc.headers.len,
                                &fc.body.data, &fc.body.len, &fc.body.cap)) {
            /* Rewrite content-type so consumers see the new charset. */
            VALUE ct_key = rb_str_new_cstr("content-type");
            VALUE ct_val = rb_hash_lookup(headers_h, ct_key);
            if (!NIL_P(ct_val)) {
                VALUE replaced = rb_funcall(ct_val, rb_intern("sub"), 2,
                    rb_reg_new_str(rb_str_new_cstr("charset\\s*=\\s*\"?[\\w\\-]+\"?"), 1 /* IGNORECASE */),
                    rb_str_new_cstr("charset=utf-8"));
                rb_hash_aset(headers_h, ct_key, replaced);
            }
        }
    }

    /* Update cache for 2xx responses with cache-relevant headers.
     * Skip when the body is empty (HEAD already exits earlier) or when
     * the response was already a cache-served 304. */
    if (cache_dir && !served_from_cache && status >= 200 && status < 300 &&
        fc.body.data && fc.body.len > 0) {
        VALUE etag_v    = rb_hash_lookup(headers_h, rb_str_new_cstr("etag"));
        VALUE lastmod_v = rb_hash_lookup(headers_h, rb_str_new_cstr("last-modified"));
        VALUE ctype_v   = rb_hash_lookup(headers_h, rb_str_new_cstr("content-type"));
        /* Only cache when there's *some* revalidation token. Otherwise the
         * entry would be useless (every fetch would always re-download). */
        if (!NIL_P(etag_v) || !NIL_P(lastmod_v)) {
            const char *etag_p    = NIL_P(etag_v)    ? "" : RSTRING_PTR(etag_v);
            size_t      etag_l    = NIL_P(etag_v)    ? 0  : (size_t)RSTRING_LEN(etag_v);
            const char *lastmod_p = NIL_P(lastmod_v) ? "" : RSTRING_PTR(lastmod_v);
            size_t      lastmod_l = NIL_P(lastmod_v) ? 0  : (size_t)RSTRING_LEN(lastmod_v);
            const char *ctype_p   = NIL_P(ctype_v)   ? "" : RSTRING_PTR(ctype_v);
            size_t      ctype_l   = NIL_P(ctype_v)   ? 0  : (size_t)RSTRING_LEN(ctype_v);
            scrap_cache_store(cache_dir, RSTRING_PTR(url_v), status,
                              etag_p, etag_l, lastmod_p, lastmod_l,
                              ctype_p, ctype_l,
                              fc.body.data, fc.body.len);
        }
    }
    scrap_cache_entry_free(&cached);

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
    int    transcode_utf8;
    long   rate_limit_ms;
    const char *user_agent;
    /* When non-zero, the worker runs dom_parse on the body and stores
     * the resulting Document in `parsed_doc`. Saves the main thread a
     * second serial pass over the batch. */
    int    parse_after_fetch;
    dom_doc_t *parsed_doc;
} pfetch_item_t;

static void pfetch_item_free(pfetch_item_t *it) {
    free(it->url);
    free(it->body);
    free(it->headers_blob);
    free(it->final_url);
}

/* Strip-and-decode Content-Encoding: takes a raw header blob + a body
 * buffer (in/out), runs the in-process decoder for the encoding the
 * server advertised, and replaces the body in place. Standalone so
 * both pfetch (pthread+easy) and mfetch (curl_multi) paths can call
 * it from no-GVL workers. */
static int scrap_decode_content_encoding(const char *headers_blob, size_t headers_len,
                                         char **body, size_t *body_len) {
    if (!headers_blob) return 1;
    const char *ce_val = NULL; size_t ce_len = 0;
    size_t i = 0;
    while (i < headers_len) {
        size_t ls = i;
        while (i < headers_len && headers_blob[i] != '\n') i++;
        size_t le = i;
        if (le > ls && headers_blob[le-1] == '\r') le--;
        if (i < headers_len) i++;
        if (le == ls) continue;
        size_t colon = (size_t)-1;
        for (size_t k = ls; k < le; k++) {
            if (headers_blob[k] == ':') { colon = k; break; }
        }
        if (colon == (size_t)-1) continue;
        if (colon - ls != 16) continue;
        const char *want = "content-encoding";
        int matches = 1;
        for (size_t k = 0; k < 16; k++) {
            char a = headers_blob[ls + k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != want[k]) { matches = 0; break; }
        }
        if (!matches) continue;
        size_t vs = colon + 1;
        while (vs < le && (headers_blob[vs] == ' ' || headers_blob[vs] == '\t')) vs++;
        ce_val = headers_blob + vs; ce_len = le - vs;
    }
    if (!ce_val) return 1;
    while (ce_len > 0 && (ce_val[ce_len-1] == ' ' || ce_val[ce_len-1] == '\t' ||
                          ce_val[ce_len-1] == '\r' || ce_val[ce_len-1] == '\n')) ce_len--;

    char *out = NULL; size_t out_len = 0;
    int decoded = 0;
#ifdef HAVE_ZLIB
    if (ce_len == 4 && ((ce_val[0] | 0x20) == 'g') && ((ce_val[1] | 0x20) == 'z') &&
        ((ce_val[2] | 0x20) == 'i') && ((ce_val[3] | 0x20) == 'p')) {
        decoded = scrap_zlib_decode(*body, *body_len, 47, &out, &out_len);
    } else if (ce_len == 7 && ((ce_val[0] | 0x20) == 'd') && ((ce_val[1] | 0x20) == 'e') &&
               ((ce_val[2] | 0x20) == 'f') && ((ce_val[3] | 0x20) == 'l') &&
               ((ce_val[4] | 0x20) == 'a') && ((ce_val[5] | 0x20) == 't') &&
               ((ce_val[6] | 0x20) == 'e')) {
        if (!scrap_zlib_decode(*body, *body_len, -15, &out, &out_len)) {
            decoded = scrap_zlib_decode(*body, *body_len, 15, &out, &out_len);
        } else decoded = 1;
    }
#endif
#ifdef HAVE_BROTLI
    if (!decoded && ce_len == 2 && ((ce_val[0] | 0x20) == 'b') && ((ce_val[1] | 0x20) == 'r')) {
        decoded = scrap_brotli_decode(*body, *body_len, &out, &out_len);
    }
#endif
#ifdef HAVE_ZSTD
    if (!decoded && ce_len == 4 && ((ce_val[0] | 0x20) == 'z') && ((ce_val[1] | 0x20) == 's') &&
        ((ce_val[2] | 0x20) == 't') && ((ce_val[3] | 0x20) == 'd')) {
        decoded = scrap_zstd_decode(*body, *body_len, &out, &out_len);
    }
#endif
    if (decoded) {
        free(*body);
        *body = out; *body_len = out_len;
    }
    return 1;
}

/* Strip Content-Encoding from a header blob (in place) and run the
 * matching in-process decoder against the body buffer. Returns 1 on
 * success or no-op, 0 on decoder failure. Thin wrapper for the
 * pfetch path which carries everything in a pfetch_item_t. */
static int pfetch_decode_body(pfetch_item_t *it) {
    return scrap_decode_content_encoding(it->headers_blob, it->headers_len,
                                          &it->body, &it->body_len);
}

static void pfetch_do_one(pfetch_item_t *it) {
    /* Per-host throttle. Gates each worker against the global slot
     * for this item's host — N parallel workers hitting one host
     * with rate_limit_ms=500 serialise at that gate while different
     * hosts run concurrently. */
    if (it->rate_limit_ms > 0) {
        char host[256];
        if (scrap_extract_host(it->url, host, sizeof(host))) {
            scrap_throttle_wait(host, (uint64_t)it->rate_limit_ms * 1000000ull);
        }
    }

    CURL *h = get_thread_curl();
    if (!h) { it->rc = CURLE_FAILED_INIT; return; }

    buf_t bbuf; memset(&bbuf, 0, sizeof(bbuf));
    buf_t hbuf; memset(&hbuf, 0, sizeof(hbuf));

    curl_easy_setopt(h, CURLOPT_URL, it->url);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    /* Tell curl to wait briefly for an existing HTTP/2 connection to
     * the target to become available rather than opening a fresh
     * TCP+TLS handshake. Combined with the shared CONNECT pool this
     * lets N workers multiplex through one connection per host. */
#ifdef CURLOPT_PIPEWAIT
    curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
#endif
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
    if (it->rc == CURLE_OK) {
        pfetch_decode_body(it);
        if (it->transcode_utf8 && it->body && it->body_len > 0) {
            size_t cap = it->body_len;  /* tracked separately just for the iconv path */
            scrap_apply_charset(it->headers_blob ? it->headers_blob : "", it->headers_len,
                                &it->body, &it->body_len, &cap);
        }
        /* Optional in-worker parse. The body buffer is handed over to a
         * dom_doc that takes ownership; we clear our pointers so the
         * post-join Ruby hash doesn't see (and free) the same memory. */
        if (it->parse_after_fetch && it->body && it->body_len > 0) {
            char *owned = it->body;
            size_t owned_len = it->body_len;
            it->body = NULL;
            it->body_len = 0;
            it->parsed_doc = scrap_dom_make_owned_doc(owned, owned_len);
            scrap_dom_parse_eager_nocache(it->parsed_doc);
        }
    }
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
    int  transcode_utf8 = 1;
    long rate_limit_ms = 0;
    int  parse_after = 0;
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
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("transcode_utf8")));
        if (!NIL_P(v)) transcode_utf8 = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("rate_limit_ms")));
        if (!NIL_P(v)) rate_limit_ms = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("parse")));
        if (!NIL_P(v)) parse_after = RTEST(v) ? 1 : 0;
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
        items[i].transcode_utf8 = transcode_utf8;
        items[i].rate_limit_ms = rate_limit_ms;
        items[i].parse_after_fetch = parse_after;
    }

    pfetch_ctx_t ctx; ctx.items = items; ctx.n = (size_t)n; ctx.next_idx = 0;
    pfetch_run_arg_t ra; ra.ctx = &ctx; ra.n_threads = n_threads;
    rb_thread_call_without_gvl(pfetch_run, &ra, NULL, NULL);

    /* Re-acquired GVL — assemble Ruby Hashes from the C results. */
    VALUE doc_klass = Qnil;
    if (parse_after) {
        doc_klass = rb_path2class("Scrapetor::Native::Document");
    }
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
        /* When the worker parsed the body, body bytes were transferred to
         * the dom_doc — the item's own body pointer is NULL. Surface the
         * Document and emit an empty body string. */
        if (it->parsed_doc) {
            rb_hash_aset(h, ID2SYM(rb_intern("document")),
                         scrap_dom_wrap_doc(doc_klass, it->parsed_doc));
            it->parsed_doc = NULL;  /* ownership transferred to the wrap */
            rb_hash_aset(h, ID2SYM(rb_intern("body")), rb_enc_str_new("", 0, enc_utf8));
        } else {
            rb_hash_aset(h, ID2SYM(rb_intern("body")),
                         rb_enc_str_new(it->body ? it->body : "", (long)it->body_len, enc_utf8));
        }
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

/* ---- curl_multi bulk fetch --------------------------------------- *
 * Single-handle curl_multi driving N concurrent transfers. Complements
 * the pthread+easy parallel_fetch path:
 *   - parallel_fetch: N pthread workers, each running its own easy
 *     handle blocking. Best when each transfer has meaningful CPU work
 *     (decode + parse) since the GVL is released across the full batch
 *     and CPU work scales with cores.
 *   - multi_fetch: one driver thread, one multi handle, N concurrent
 *     transfers multiplexed via curl_multi_perform. Best for
 *     I/O-dominated high-fan-out fetches (hundreds of URLs across
 *     diverse hosts) where the cost of pthread setup outweighs the
 *     in-flight transfer count.
 *
 * Both share the same global CURLSH, so connection pool / DNS / TLS
 * sessions are shared across them too.
 */

typedef struct {
    char       *url;
    CURL       *easy;
    buf_t       body;
    buf_t       headers;
    long        status;
    long        http_version;
    char       *final_url;       /* strdup */
    CURLcode    rc;
    char        errstr[CURL_ERROR_SIZE];
    struct curl_slist *req_headers;  /* per-easy slist, freed after harvest */
    /* In-loop decode/parse output. Populated by the perform thread
     * as each transfer completes — keeps the per-completion CPU work
     * (decompress + transcode + tokenise) inside the same no-GVL
     * window. */
    int         decoded;          /* 1 after we've drained the message for this slot */
    dom_doc_t  *parsed_doc;       /* optional, set when parse_after */
    /* HTTP cache: populated pre-perform from disk lookup; checked
     * post-perform for 304 revalidation. */
    scrap_cache_entry_t cached;
    int         have_cached;
    int         served_from_cache;
} mfetch_slot_t;

typedef struct {
    CURLM         *multi;
    mfetch_slot_t *slots;
    size_t         n;
    CURLMcode      multi_rc;
    int            transcode_utf8;
    int            parse_after;
    const char    *cache_dir;
} mfetch_ctx_t;

static void mfetch_finalize_slot_nogvl(mfetch_ctx_t *ctx, mfetch_slot_t *s,
                                       CURL *easy, CURLcode result) {
    s->rc = result;
    if (result != CURLE_OK) { s->decoded = 1; return; }
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &s->status);
    curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &s->http_version);
    char *eff = NULL;
    curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) {
        size_t l = strlen(eff);
        s->final_url = (char *)malloc(l + 1);
        memcpy(s->final_url, eff, l + 1);
    }
    /* 304 revalidation: server says cached body still valid. Swap
     * the body buffer for the cached payload and rewrite status to
     * 200 so consumers see a fully-formed response. */
    if (ctx->cache_dir && s->have_cached && s->status == 304) {
        free(s->body.data);
        s->body.data = (char *)malloc(s->cached.body_len + 1);
        memcpy(s->body.data, s->cached.body, s->cached.body_len);
        s->body.data[s->cached.body_len] = 0;
        s->body.len = s->cached.body_len;
        s->body.cap = s->cached.body_len;
        s->status = 200;
        s->served_from_cache = 1;
    }
    /* Decompress + transcode under no-GVL. */
    if (s->body.data && s->body.len > 0) {
        scrap_decode_content_encoding(s->headers.data ? s->headers.data : "",
                                       s->headers.len, &s->body.data, &s->body.len);
    }
    if (ctx->transcode_utf8 && s->body.data && s->body.len > 0) {
        size_t cap = s->body.len;
        scrap_apply_charset(s->headers.data ? s->headers.data : "", s->headers.len,
                            &s->body.data, &s->body.len, &cap);
        s->body.cap = cap;
    }
    /* Optional in-loop parse — same trick as parallel_fetch: hand
     * ownership of the body buffer to a dom_doc_t and run
     * dom_parse_eager_nocache. */
    if (ctx->parse_after && s->body.data && s->body.len > 0) {
        char *owned = s->body.data;
        size_t owned_len = s->body.len;
        s->body.data = NULL;
        s->body.len = 0;
        s->parsed_doc = scrap_dom_make_owned_doc(owned, owned_len);
        scrap_dom_parse_eager_nocache(s->parsed_doc);
    }
    s->decoded = 1;
}

static void *mfetch_run_nogvl(void *arg) {
    mfetch_ctx_t *ctx = (mfetch_ctx_t *)arg;
    int running = -1;
    while (1) {
        ctx->multi_rc = curl_multi_perform(ctx->multi, &running);
        if (ctx->multi_rc != CURLM_OK) break;

        /* Drain completed messages now so decompression / transcode /
         * parse runs in parallel with other in-flight transfers
         * (still on this same driver thread, but interleaved with
         * curl_multi_perform). */
        CURLMsg *msg;
        int q;
        while ((msg = curl_multi_info_read(ctx->multi, &q))) {
            if (msg->msg != CURLMSG_DONE) continue;
            long idx = -1;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &idx);
            if (idx < 0 || idx >= (long)ctx->n) continue;
            mfetch_slot_t *s = &ctx->slots[idx];
            if (s->decoded) continue;
            mfetch_finalize_slot_nogvl(ctx, s, msg->easy_handle, msg->data.result);
        }

        if (running == 0) break;
        int numfds = 0;
        curl_multi_poll(ctx->multi, NULL, 0, 200, &numfds);
    }
    return NULL;
}

static VALUE scrap_multi_fetch(int argc, VALUE *argv, VALUE self) {
    (void)self;
    VALUE urls_v, opts_v;
    rb_scan_args(argc, argv, "11", &urls_v, &opts_v);
    Check_Type(urls_v, T_ARRAY);
    long n = RARRAY_LEN(urls_v);
    if (n == 0) return rb_ary_new();

    long timeout_ms = 30000;
    int  follow = 1;
    long max_redirs = 10;
    const char *ua = "scrapetor/0.1 (libcurl)";
    int  insecure = 0;
    long max_concurrent = 0;   /* 0 = no cap (let multi run as wide as needed) */
    int  transcode_utf8 = 1;
    int  parse_after = 0;
    const char *cache_dir = NULL;
    const char *method_opt = NULL;
    int  nobody_opt = 0;
    VALUE headers_v = Qnil;
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
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("insecure")));
        if (!NIL_P(v)) insecure = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("headers")));
        if (!NIL_P(v)) { Check_Type(v, T_HASH); headers_v = v; }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("max_concurrent")));
        if (!NIL_P(v)) max_concurrent = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("transcode_utf8")));
        if (!NIL_P(v)) transcode_utf8 = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("parse")));
        if (!NIL_P(v)) parse_after = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("cache_dir")));
        if (!NIL_P(v)) { Check_Type(v, T_STRING); cache_dir = RSTRING_PTR(v); }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("method")));
        if (!NIL_P(v)) {
            if (SYMBOL_P(v)) v = rb_sym2str(v);
            Check_Type(v, T_STRING);
            method_opt = RSTRING_PTR(v);
            if (strcasecmp(method_opt, "head") == 0) { nobody_opt = 1; method_opt = NULL; }
            else if (strcasecmp(method_opt, "get") == 0) method_opt = NULL;
        }
    }

    CURLM *multi = curl_multi_init();
    if (max_concurrent > 0) {
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_concurrent);
    }
#ifdef CURLPIPE_MULTIPLEX
    /* Let multi pile new requests onto an existing HTTP/2 connection
     * to the same host. With CURLOPT_PIPEWAIT also set per-handle, the
     * multi pool tends to settle on one connection per origin. */
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, (long)CURLPIPE_MULTIPLEX);
#endif

    mfetch_slot_t *slots = (mfetch_slot_t *)calloc((size_t)n, sizeof(mfetch_slot_t));

    for (long i = 0; i < n; i++) {
        VALUE u = rb_ary_entry(urls_v, i);
        Check_Type(u, T_STRING);
        size_t ul = (size_t)RSTRING_LEN(u);
        slots[i].url = (char *)malloc(ul + 1);
        memcpy(slots[i].url, RSTRING_PTR(u), ul);
        slots[i].url[ul] = 0;

        CURL *h = curl_easy_init();
        slots[i].easy = h;
        curl_easy_setopt(h, CURLOPT_URL, slots[i].url);
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
#ifdef CURLOPT_PIPEWAIT
        curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
#endif
        curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, (long)follow);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, max_redirs);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, cb_body);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &slots[i].body);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, cb_header);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &slots[i].headers);
        curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, slots[i].errstr);
        curl_easy_setopt(h, CURLOPT_PRIVATE, (void *)(intptr_t)i);
        if (g_share) curl_easy_setopt(h, CURLOPT_SHARE, g_share);
        if (insecure) {
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        if (nobody_opt) {
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD");
        } else if (method_opt) {
            /* Upcase + custom-request for non-GET. */
            char mbuf[24];
            size_t mi = 0;
            for (; mi < sizeof(mbuf) - 1 && method_opt[mi]; mi++) {
                char c = method_opt[mi];
                mbuf[mi] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            mbuf[mi] = 0;
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, mbuf);
        }
        /* HTTP cache: pre-load entry for this URL so we can attach
         * If-None-Match / If-Modified-Since and identify 304s in the
         * worker. HEAD is allowed here because the revalidate flow
         * uses HEAD specifically to ping the server about freshness;
         * non-GET methods other than HEAD (POST/PUT/DELETE/...) are
         * skipped per RFC 7234. */
        if (cache_dir && !method_opt) {
            slots[i].have_cached = scrap_cache_load(cache_dir, slots[i].url, &slots[i].cached);
        }
        /* Per-handle Accept-Encoding + user headers slist. */
        {
            char ae_line[160];
            snprintf(ae_line, sizeof(ae_line), "Accept-Encoding: %s",
                     scrap_accept_encoding());
            slots[i].req_headers = curl_slist_append(slots[i].req_headers, ae_line);
        }
        if (slots[i].have_cached) {
            if (slots[i].cached.etag_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-None-Match: %.*s",
                         (int)slots[i].cached.etag_len, slots[i].cached.etag);
                slots[i].req_headers = curl_slist_append(slots[i].req_headers, line);
            }
            if (slots[i].cached.lastmod_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-Modified-Since: %.*s",
                         (int)slots[i].cached.lastmod_len, slots[i].cached.lastmod);
                slots[i].req_headers = curl_slist_append(slots[i].req_headers, line);
            }
        }
        if (!NIL_P(headers_v)) {
            VALUE keys = rb_funcall(headers_v, rb_intern("keys"), 0);
            long nk = RARRAY_LEN(keys);
            for (long k = 0; k < nk; k++) {
                VALUE kk = rb_ary_entry(keys, k);
                VALUE vv = rb_hash_aref(headers_v, kk);
                VALUE line = rb_str_dup(kk);
                rb_str_cat_cstr(line, ": ");
                rb_str_append(line, vv);
                slots[i].req_headers = curl_slist_append(slots[i].req_headers, RSTRING_PTR(line));
            }
        }
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, slots[i].req_headers);
        curl_multi_add_handle(multi, h);
    }

    mfetch_ctx_t ctx;
    ctx.multi = multi;
    ctx.slots = slots;
    ctx.n = (size_t)n;
    ctx.multi_rc = CURLM_OK;
    ctx.transcode_utf8 = transcode_utf8;
    ctx.parse_after = parse_after;
    ctx.cache_dir = cache_dir;
    rb_thread_call_without_gvl(mfetch_run_nogvl, &ctx, NULL, NULL);

    /* Sweep any final messages the worker didn't drain (defensive —
     * the worker loop normally consumes them all, but if the multi
     * exited via error or the exit condition fired between perform
     * and info_read, a message could still be queued). */
    {
        CURLMsg *msg;
        int q;
        while ((msg = curl_multi_info_read(multi, &q))) {
            if (msg->msg != CURLMSG_DONE) continue;
            long idx = -1;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &idx);
            if (idx < 0 || idx >= (long)n) continue;
            if (!slots[idx].decoded) {
                mfetch_finalize_slot_nogvl(&ctx, &slots[idx], msg->easy_handle, msg->data.result);
            }
        }
    }

    /* All slots have been decoded by the worker (or by the sweep
     * above). The harvest pass just builds the Ruby surface. */
    VALUE doc_klass = parse_after ? rb_path2class("Scrapetor::Native::Document") : Qnil;
    VALUE result = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        mfetch_slot_t *s = &slots[i];
        VALUE h = rb_hash_new();
        if (s->rc != CURLE_OK) {
            VALUE err = rb_hash_new();
            rb_hash_aset(err, ID2SYM(rb_intern("url")), rb_str_new_cstr(s->url));
            rb_hash_aset(err, ID2SYM(rb_intern("error")),
                         rb_str_new_cstr(s->errstr[0] ? s->errstr : curl_easy_strerror(s->rc)));
            rb_hash_aset(h, ID2SYM(rb_intern("error")), err);
        } else {
            rb_hash_aset(h, ID2SYM(rb_intern("status")), LONG2NUM(s->status));
            if (s->parsed_doc) {
                rb_hash_aset(h, ID2SYM(rb_intern("document")),
                             scrap_dom_wrap_doc(doc_klass, s->parsed_doc));
                s->parsed_doc = NULL;
                rb_hash_aset(h, ID2SYM(rb_intern("body")), rb_enc_str_new("", 0, enc_utf8));
            } else {
                rb_hash_aset(h, ID2SYM(rb_intern("body")),
                             rb_enc_str_new(s->body.data ? s->body.data : "",
                                            (long)s->body.len, enc_utf8));
            }
            VALUE hh = parse_headers_blob(s->headers.data ? s->headers.data : "",
                                          s->headers.len);
            rb_hash_delete(hh, rb_str_new_cstr("content-encoding"));
            if (s->served_from_cache && s->cached.ctype_len > 0) {
                rb_hash_aset(hh, rb_str_new_cstr("content-type"),
                             rb_str_new(s->cached.ctype, (long)s->cached.ctype_len));
                rb_hash_aset(hh, rb_str_new_cstr("x-scrapetor-cache"),
                             rb_str_new_cstr("hit"));
            } else if (cache_dir && s->have_cached) {
                rb_hash_aset(hh, rb_str_new_cstr("x-scrapetor-cache"),
                             rb_str_new_cstr("miss-revalidated"));
            }
            rb_hash_aset(h, ID2SYM(rb_intern("headers")), hh);
            rb_hash_aset(h, ID2SYM(rb_intern("final_url")),
                         rb_str_new_cstr(s->final_url ? s->final_url : s->url));
            const char *hv_str = "1.1";
            switch (s->http_version) {
                case CURL_HTTP_VERSION_1_0: hv_str = "1.0"; break;
                case CURL_HTTP_VERSION_1_1: hv_str = "1.1"; break;
                case CURL_HTTP_VERSION_2_0: hv_str = "2";   break;
#ifdef CURL_HTTP_VERSION_3
                case CURL_HTTP_VERSION_3:   hv_str = "3";   break;
#endif
            }
            rb_hash_aset(h, ID2SYM(rb_intern("http_version")), rb_str_new_cstr(hv_str));
            /* Store the response in cache for next-time revalidation
             * (only for cache-eligible 2xx responses with a token). */
            if (cache_dir && !s->served_from_cache && s->status >= 200 && s->status < 300 &&
                s->body.data && s->body.len > 0) {
                VALUE etag_v    = rb_hash_lookup(hh, rb_str_new_cstr("etag"));
                VALUE lastmod_v = rb_hash_lookup(hh, rb_str_new_cstr("last-modified"));
                VALUE ctype_v   = rb_hash_lookup(hh, rb_str_new_cstr("content-type"));
                if (!NIL_P(etag_v) || !NIL_P(lastmod_v)) {
                    const char *etag_p    = NIL_P(etag_v)    ? "" : RSTRING_PTR(etag_v);
                    size_t      etag_l    = NIL_P(etag_v)    ? 0  : (size_t)RSTRING_LEN(etag_v);
                    const char *lastmod_p = NIL_P(lastmod_v) ? "" : RSTRING_PTR(lastmod_v);
                    size_t      lastmod_l = NIL_P(lastmod_v) ? 0  : (size_t)RSTRING_LEN(lastmod_v);
                    const char *ctype_p   = NIL_P(ctype_v)   ? "" : RSTRING_PTR(ctype_v);
                    size_t      ctype_l   = NIL_P(ctype_v)   ? 0  : (size_t)RSTRING_LEN(ctype_v);
                    scrap_cache_store(cache_dir, s->url, s->status,
                                      etag_p, etag_l, lastmod_p, lastmod_l,
                                      ctype_p, ctype_l, s->body.data, s->body.len);
                }
            }
        }
        rb_ary_push(result, h);

        curl_multi_remove_handle(multi, s->easy);
        curl_easy_cleanup(s->easy);
        curl_slist_free_all(s->req_headers);
        free(s->url);
        free(s->body.data);
        free(s->headers.data);
        free(s->final_url);
        scrap_cache_entry_free(&s->cached);
    }
    curl_multi_cleanup(multi);
    free(slots);
    return result;
}

/* ---- streaming multi batch (yield as transfers complete) --------- *
 * Wraps a CURLM handle + slots in a typed-data object so Ruby can pull
 * completed responses one at a time via #next. Each #next advances
 * curl_multi_perform under no-GVL until at least one new transfer
 * completes, finalises that slot (decompress / transcode / optional
 * parse), and returns its Ruby hash. nil when the whole batch is done.
 *
 * Pattern: Fetcher.multi_each(urls) { |r| ... } yields each response
 * in completion order — earliest-arriving first — so the user starts
 * processing while later transfers are still on the wire.
 */
typedef struct {
    CURLM *multi;
    mfetch_slot_t *slots;
    size_t n;
    /* Completion ring: indices of slots that finished and aren't
     * yielded yet. ready_tail bumps in the worker, ready_head bumps
     * on each #next pop. */
    size_t *ready_queue;
    size_t  ready_head;
    size_t  ready_tail;
    int     running;
    int     done;
    /* Carried opts (mirrors mfetch_ctx_t shape so we can reuse
     * mfetch_finalize_slot_nogvl). */
    int     transcode_utf8;
    int     parse_after;
    char   *cache_dir_owned;   /* strdup, may be NULL */
    /* Whole-batch shared slist for Accept-Encoding + user headers.
     * Owned; freed at cleanup. */
    struct curl_slist *shared_headers;
    /* All easy handles also live here so we can free them on GC. */
} mbatch_t;

static void mbatch_free(void *p) {
    mbatch_t *b = (mbatch_t *)p;
    if (!b) return;
    if (b->slots) {
        for (size_t i = 0; i < b->n; i++) {
            mfetch_slot_t *s = &b->slots[i];
            if (s->easy) {
                if (b->multi) curl_multi_remove_handle(b->multi, s->easy);
                curl_easy_cleanup(s->easy);
            }
            curl_slist_free_all(s->req_headers);
            free(s->url);
            free(s->body.data);
            free(s->headers.data);
            free(s->final_url);
            scrap_cache_entry_free(&s->cached);
            if (s->parsed_doc) {
                /* parsed_doc may not have been yielded yet — its bytes
                 * are owned by the dom_doc so just let it leak through
                 * the parse-doc free path. */
                /* No direct free here; the dom_doc's own free handles it
                 * once the wrap is GC'd. Without a wrap, it leaks. */
            }
        }
        free(b->slots);
    }
    if (b->multi) curl_multi_cleanup(b->multi);
    free(b->ready_queue);
    free(b->cache_dir_owned);
    free(b);
}

static size_t mbatch_memsize(const void *p) {
    const mbatch_t *b = (const mbatch_t *)p;
    return sizeof(*b) + (b ? b->n * sizeof(mfetch_slot_t) : 0);
}

static const rb_data_type_t mbatch_data_type = {
    "Scrapetor::Native::Http::MultiBatch",
    {NULL, mbatch_free, mbatch_memsize},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE mbatch_alloc(VALUE klass) {
    mbatch_t *b = (mbatch_t *)calloc(1, sizeof(mbatch_t));
    return TypedData_Wrap_Struct(klass, &mbatch_data_type, b);
}

/* No-GVL stepper: one perform call + drain any completed messages,
 * finalising each as it lands. May poll for socket activity if no
 * completion is ready yet. */
static void *mbatch_step_nogvl(void *arg) {
    mbatch_t *b = (mbatch_t *)arg;
    curl_multi_perform(b->multi, &b->running);
    CURLMsg *msg;
    int q;
    while ((msg = curl_multi_info_read(b->multi, &q))) {
        if (msg->msg != CURLMSG_DONE) continue;
        long idx = -1;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &idx);
        if (idx < 0 || idx >= (long)b->n) continue;
        if (b->slots[idx].decoded) continue;
        mfetch_ctx_t ctx_proxy;
        memset(&ctx_proxy, 0, sizeof(ctx_proxy));
        ctx_proxy.transcode_utf8 = b->transcode_utf8;
        ctx_proxy.parse_after = b->parse_after;
        ctx_proxy.cache_dir = b->cache_dir_owned;
        mfetch_finalize_slot_nogvl(&ctx_proxy, &b->slots[idx],
                                    msg->easy_handle, msg->data.result);
        b->ready_queue[b->ready_tail++] = (size_t)idx;
    }
    if (b->ready_head >= b->ready_tail && b->running > 0) {
        int numfds = 0;
        curl_multi_poll(b->multi, NULL, 0, 200, &numfds);
    }
    if (b->running == 0) b->done = 1;
    return NULL;
}

/* Build the Ruby Hash for a finalised slot. Same shape as
 * scrap_multi_fetch's harvest path. */
static VALUE mbatch_build_hash(mbatch_t *b, mfetch_slot_t *s) {
    VALUE h = rb_hash_new();
    if (s->rc != CURLE_OK) {
        VALUE err = rb_hash_new();
        rb_hash_aset(err, ID2SYM(rb_intern("url")), rb_str_new_cstr(s->url));
        rb_hash_aset(err, ID2SYM(rb_intern("error")),
                     rb_str_new_cstr(s->errstr[0] ? s->errstr : curl_easy_strerror(s->rc)));
        rb_hash_aset(h, ID2SYM(rb_intern("error")), err);
        return h;
    }
    rb_hash_aset(h, ID2SYM(rb_intern("status")), LONG2NUM(s->status));
    if (s->parsed_doc) {
        VALUE doc_klass = rb_path2class("Scrapetor::Native::Document");
        rb_hash_aset(h, ID2SYM(rb_intern("document")),
                     scrap_dom_wrap_doc(doc_klass, s->parsed_doc));
        s->parsed_doc = NULL;
        rb_hash_aset(h, ID2SYM(rb_intern("body")), rb_enc_str_new("", 0, enc_utf8));
    } else {
        rb_hash_aset(h, ID2SYM(rb_intern("body")),
                     rb_enc_str_new(s->body.data ? s->body.data : "",
                                    (long)s->body.len, enc_utf8));
    }
    VALUE hh = parse_headers_blob(s->headers.data ? s->headers.data : "",
                                  s->headers.len);
    rb_hash_delete(hh, rb_str_new_cstr("content-encoding"));
    if (s->served_from_cache && s->cached.ctype_len > 0) {
        rb_hash_aset(hh, rb_str_new_cstr("content-type"),
                     rb_str_new(s->cached.ctype, (long)s->cached.ctype_len));
        rb_hash_aset(hh, rb_str_new_cstr("x-scrapetor-cache"),
                     rb_str_new_cstr("hit"));
    } else if (b->cache_dir_owned && s->have_cached) {
        rb_hash_aset(hh, rb_str_new_cstr("x-scrapetor-cache"),
                     rb_str_new_cstr("miss-revalidated"));
    }
    rb_hash_aset(h, ID2SYM(rb_intern("headers")), hh);
    rb_hash_aset(h, ID2SYM(rb_intern("final_url")),
                 rb_str_new_cstr(s->final_url ? s->final_url : s->url));
    const char *hv_str = "1.1";
    switch (s->http_version) {
        case CURL_HTTP_VERSION_1_0: hv_str = "1.0"; break;
        case CURL_HTTP_VERSION_1_1: hv_str = "1.1"; break;
        case CURL_HTTP_VERSION_2_0: hv_str = "2";   break;
#ifdef CURL_HTTP_VERSION_3
        case CURL_HTTP_VERSION_3:   hv_str = "3";   break;
#endif
    }
    rb_hash_aset(h, ID2SYM(rb_intern("http_version")), rb_str_new_cstr(hv_str));
    return h;
}

static VALUE mbatch_initialize(int argc, VALUE *argv, VALUE self) {
    VALUE urls_v, opts_v;
    rb_scan_args(argc, argv, "11", &urls_v, &opts_v);
    Check_Type(urls_v, T_ARRAY);
    long n = RARRAY_LEN(urls_v);

    mbatch_t *b;
    TypedData_Get_Struct(self, mbatch_t, &mbatch_data_type, b);

    long timeout_ms = 30000;
    int  follow = 1;
    long max_redirs = 10;
    const char *ua = "scrapetor/0.1 (libcurl)";
    int  insecure = 0;
    long max_concurrent = 0;
    b->transcode_utf8 = 1;
    b->parse_after = 0;
    VALUE headers_v = Qnil;
    const char *method_opt = NULL;
    int  nobody_opt = 0;
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
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("insecure")));
        if (!NIL_P(v)) insecure = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("headers")));
        if (!NIL_P(v)) { Check_Type(v, T_HASH); headers_v = v; }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("max_concurrent")));
        if (!NIL_P(v)) max_concurrent = NUM2LONG(v);
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("transcode_utf8")));
        if (!NIL_P(v)) b->transcode_utf8 = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("parse")));
        if (!NIL_P(v)) b->parse_after = RTEST(v) ? 1 : 0;
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("cache_dir")));
        if (!NIL_P(v)) {
            Check_Type(v, T_STRING);
            size_t l = (size_t)RSTRING_LEN(v);
            b->cache_dir_owned = (char *)malloc(l + 1);
            memcpy(b->cache_dir_owned, RSTRING_PTR(v), l);
            b->cache_dir_owned[l] = 0;
        }
        v = rb_hash_aref(opts_v, ID2SYM(rb_intern("method")));
        if (!NIL_P(v)) {
            if (SYMBOL_P(v)) v = rb_sym2str(v);
            Check_Type(v, T_STRING);
            method_opt = RSTRING_PTR(v);
            if (strcasecmp(method_opt, "head") == 0) { nobody_opt = 1; method_opt = NULL; }
            else if (strcasecmp(method_opt, "get") == 0) method_opt = NULL;
        }
    }

    b->n = (size_t)n;
    b->multi = curl_multi_init();
    if (max_concurrent > 0) {
        curl_multi_setopt(b->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_concurrent);
    }
#ifdef CURLPIPE_MULTIPLEX
    curl_multi_setopt(b->multi, CURLMOPT_PIPELINING, (long)CURLPIPE_MULTIPLEX);
#endif
    b->slots = (mfetch_slot_t *)calloc(b->n, sizeof(mfetch_slot_t));
    b->ready_queue = (size_t *)calloc(b->n, sizeof(size_t));

    for (long i = 0; i < n; i++) {
        VALUE u = rb_ary_entry(urls_v, i);
        Check_Type(u, T_STRING);
        size_t ul = (size_t)RSTRING_LEN(u);
        b->slots[i].url = (char *)malloc(ul + 1);
        memcpy(b->slots[i].url, RSTRING_PTR(u), ul);
        b->slots[i].url[ul] = 0;

        CURL *h = curl_easy_init();
        b->slots[i].easy = h;
        curl_easy_setopt(h, CURLOPT_URL, b->slots[i].url);
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
#ifdef CURLOPT_PIPEWAIT
        curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
#endif
        curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, (long)follow);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, max_redirs);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, cb_body);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &b->slots[i].body);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, cb_header);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &b->slots[i].headers);
        curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, b->slots[i].errstr);
        curl_easy_setopt(h, CURLOPT_PRIVATE, (void *)(intptr_t)i);
        if (g_share) curl_easy_setopt(h, CURLOPT_SHARE, g_share);
        if (insecure) {
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        if (nobody_opt) {
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD");
        } else if (method_opt) {
            char mbuf[24];
            size_t mi = 0;
            for (; mi < sizeof(mbuf) - 1 && method_opt[mi]; mi++) {
                char c = method_opt[mi];
                mbuf[mi] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            mbuf[mi] = 0;
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, mbuf);
        }
        if (b->cache_dir_owned && !method_opt) {
            b->slots[i].have_cached =
                scrap_cache_load(b->cache_dir_owned, b->slots[i].url, &b->slots[i].cached);
        }
        {
            char ae_line[160];
            snprintf(ae_line, sizeof(ae_line), "Accept-Encoding: %s",
                     scrap_accept_encoding());
            b->slots[i].req_headers = curl_slist_append(b->slots[i].req_headers, ae_line);
        }
        if (b->slots[i].have_cached) {
            if (b->slots[i].cached.etag_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-None-Match: %.*s",
                         (int)b->slots[i].cached.etag_len, b->slots[i].cached.etag);
                b->slots[i].req_headers = curl_slist_append(b->slots[i].req_headers, line);
            }
            if (b->slots[i].cached.lastmod_len > 0) {
                char line[1024];
                snprintf(line, sizeof(line), "If-Modified-Since: %.*s",
                         (int)b->slots[i].cached.lastmod_len, b->slots[i].cached.lastmod);
                b->slots[i].req_headers = curl_slist_append(b->slots[i].req_headers, line);
            }
        }
        if (!NIL_P(headers_v)) {
            VALUE keys = rb_funcall(headers_v, rb_intern("keys"), 0);
            long nk = RARRAY_LEN(keys);
            for (long k = 0; k < nk; k++) {
                VALUE kk = rb_ary_entry(keys, k);
                VALUE vv = rb_hash_aref(headers_v, kk);
                VALUE line = rb_str_dup(kk);
                rb_str_cat_cstr(line, ": ");
                rb_str_append(line, vv);
                b->slots[i].req_headers = curl_slist_append(b->slots[i].req_headers, RSTRING_PTR(line));
            }
        }
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, b->slots[i].req_headers);
        curl_multi_add_handle(b->multi, h);
    }
    b->running = (int)b->n;
    return self;
}

static VALUE mbatch_next(VALUE self) {
    mbatch_t *b;
    TypedData_Get_Struct(self, mbatch_t, &mbatch_data_type, b);
    while (b->ready_head >= b->ready_tail && !b->done) {
        rb_thread_call_without_gvl(mbatch_step_nogvl, b, NULL, NULL);
    }
    if (b->ready_head >= b->ready_tail) return Qnil;
    size_t idx = b->ready_queue[b->ready_head++];
    return mbatch_build_hash(b, &b->slots[idx]);
}

void Init_scrapetor_http(VALUE mod_native) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    scrap_share_init();
    VALUE mod_http = rb_define_module_under(mod_native, "Http");
    rb_define_singleton_method(mod_http, "get",            scrap_http_get,         -1);
    rb_define_singleton_method(mod_http, "parallel_fetch", scrap_parallel_fetch,   -1);
    rb_define_singleton_method(mod_http, "multi_fetch",    scrap_multi_fetch,      -1);
    rb_define_singleton_method(mod_http, "features",       scrap_http_features,     0);
    rb_define_const(mod_http, "AVAILABLE", Qtrue);

    /* Streaming multi-batch iterator. */
    VALUE mb = rb_define_class_under(mod_http, "MultiBatch", rb_cObject);
    rb_define_alloc_func(mb, mbatch_alloc);
    rb_define_method(mb, "initialize", mbatch_initialize, -1);
    rb_define_method(mb, "next",       mbatch_next, 0);
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
