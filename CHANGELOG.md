# Changelog

All notable changes to Scrapetor are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/).

## [0.2.0]

The 0.2 series turns Scrapetor from a parser-plus-Net::HTTP-helper
into a production-shaped scraping toolkit. Every load-bearing piece
is native C with the GVL released across network + decode + parse.

### Added — HTTP layer (libcurl-backed, opt-in at build time)

- `Scrapetor::Fetcher.get` / `.post` / `.put` / `.patch` / `.delete` /
  `.head`. HTTP/2 over TLS via ALPN with graceful 1.1 fallback;
  per-thread libcurl handle cache; GVL released for the round-trip.
- Request body sugar: `:json`, `:form`, `:body`, `:multipart`. Multipart
  parts can come from `:path` (file) or `:data` (bytes), with optional
  `:filename` / `:content_type` overrides. Driven via `curl_mime`.
- In-process content-encoding decoders. Advertised in the
  `Accept-Encoding` header and decoded by Scrapetor regardless of
  whether the linked libcurl was built with the codec:
  - gzip + deflate via zlib
  - brotli via libbrotlidec
  - zstd via libzstd
- Charset transcoding to UTF-8 via iconv. Parses `charset=...` out of
  `Content-Type`, transcodes the body, rewrites the header. Latin-1,
  Shift_JIS, etc. round-trip end-to-end. Opt-out via
  `transcode_utf8: false`.
- Per-host throttle in native code. `:rate_limit_ms` honours the same
  gate across single + parallel + multi paths via a global pthread-
  mutexed host → next-allowed-time table.
- Retry + exponential backoff with full jitter. `:retry`, `:backoff`,
  `:max_backoff`, `:retry_on`. Honours numeric `Retry-After` response
  headers (capped at `max_backoff`).
- Auth: `:basic_auth` (`"user:pass"` → CURLAUTH_BASIC) and
  `:bearer_token` (CURLAUTH_BEARER + CURLOPT_XOAUTH2_BEARER, with an
  Authorization-header fallback on older libcurl).
- Proxy + custom CA: `:proxy`, `:ca_path`, `:insecure`.
- ETag / Last-Modified disk cache. `:cache_dir` opts in; second-and-
  later requests send `If-None-Match` / `If-Modified-Since`; 304
  swaps in the cached body and marks
  `headers["x-scrapetor-cache"] = "hit"`.
  - `Scrapetor::Fetcher.revalidate(urls, cache_dir:)` HEADs every URL
    in one curl_multi sweep and classifies each as `:fresh` /
    `:changed` / `:missing` / `:error`.

### Added — Bulk fetch concurrency models

- `Scrapetor::Fetcher.parallel_get` / `.parallel_fetch`: N pthread
  workers each running blocking easy handles. Best when each
  response has CPU work after the fetch (decode + parse), since the
  GVL is released for the whole batch. `parallel_fetch` runs the
  full pipeline — decode + transcode + dom_parse + index build —
  inside the same no-GVL window.
- `Scrapetor::Fetcher.multi_get` / `.multi_fetch`: single driver
  thread + `curl_multi`, N concurrent in-flight. Best for I/O-
  fan-out (hundreds of URLs across many hosts). Same `parse: true`
  / `cache_dir:` options as `parallel_fetch`.
- `Scrapetor::Fetcher.multi_each(urls) { |r| ... }`: streaming
  variant. Yields each response in *completion* order (not input
  order) as soon as it's done, so processing can start while later
  transfers are still on the wire. Backed by a `MultiBatch`
  typed-data iterator.
- `CURLSH` shared connection pool + DNS + TLS session cache across
  every pthread worker and every multi handle. N workers hitting
  one host now reuse one connection, DNS resolve, and TLS session.
  `CURLOPT_PIPEWAIT` per-handle for HTTP/2 multiplexing.

### Added — Stateful clients

- `Scrapetor::Session.new(cookies: …, user_agent:, rate_limit:,
  retry:, headers:, basic_auth:, bearer_token:, proxy:, ca_path:)`.
  Wraps Fetcher with persistent cookie jar (libcurl COOKIEJAR/
  COOKIEFILE; ephemeral tempfile by default), default-header merge,
  auto-applied auth, per-host throttle, default retry policy. Verbs:
  `get`/`post`/`put`/`patch`/`delete`/`head`/`fetch` (parsed
  Document)/`parallel_get`.

### Added — Parser improvements

- Persistent disk-backed parse cache (`SCRAP_PERSISTENT_CACHE=1`).
  Binary arena dump on disk, indexes rebuilt on load, content-
  addressed via SHA-256.
- Tag-name interning. Static intern table → `uint16_t tag_id` per
  node (free, slots into existing struct padding). Per-match
  `==` replaces strncasecmp for the standard tag set.
- Ancestor bloom filter (64-bit per node, content-hashed). Chain
  matcher fast-rejects candidates whose ancestor_bloom doesn't
  cover the required mask.
- Selector specialisation. `c_atom` carries a function pointer to
  one of `match_class_only` / `match_tag_only` / `match_tag_class` /
  `match_id_only` chosen at plan compile, skipping the predicate
  dispatch loop for the common shapes.
- NEON SIMD scanners on arm64: `dom_advance_ws`, `dom_advance_name`,
  `dom_advance_attr_end`, NEON-accelerated `dom_streq_ci` for
  index-lookup compares, NEON-driven class-attr tokeniser. Remaining
  byte loops in `dom_parse` hoisted onto libc's SIMD `memchr`.
- Cold parse benchmark vs Nokolexbor: 0.40 ms vs 0.39 ms (was
  0.54 ms before this series).

### Added — Bounded-memory parsing

- `Scrapetor.parallel_parse(htmls, threads:)`. Real multi-core HTML
  parsing on MRI: pthread workers + `rb_thread_call_without_gvl`.
- `Scrapetor.stream(io, outer:)`. Streaming row parser with depth-
  tracked nested-same-tag balancing, `<script>` / `<style>` /
  comment / CDATA skipping, all in C. Outer pattern accepts `tag`,
  `tag.class`, `tag.cls1.cls2`, `tag#id`, `tag#id.cls`. Peak memory
  is bounded to `max(chunk_size, longest_row_in_bytes)` regardless
  of total document size.

### Added — Crawl helpers

- `Scrapetor::Robots.fetch_for(host)` — robots.txt parser with
  longest-match decision (RFC 9309 / Google's de-facto rule), `*`
  and `$` patterns, case-insensitive UA prefix matching,
  Crawl-delay + Sitemap directives.
- `Scrapetor::Sitemap.urls(source)` — streaming sitemap.xml
  enumerator with recursion into `<sitemapindex>`. Accepts IO,
  String XML, or URL String.

### Added — Tests + benchmarks

- 74 new tests across 6 new files: `test_robots.rb`,
  `test_sitemap.rb`, `test_stream_parser.rb`, `test_fetcher.rb`,
  `test_session.rb`, `test_parallel_parse.rb`,
  `test_persistent_cache.rb`. Fetcher + Session tests auto-skip on
  builds without libcurl. 291 tests total (was 158), all green.
- `benchmark/end_to_end.rb` — fetch+parse+extract vs Net::HTTP +
  Nokogiri and Net::HTTP + Nokolexbor on a local WEBrick server.
  Reproducible without network access.

### Changed

- `Scrapetor.parse` consults the persistent parse cache when
  `SCRAP_PERSISTENT_CACHE=1` is set, otherwise behaviour is
  unchanged.
- Persistent cache binary format magic bumped from `SCRAPV01` to
  `SCRAPV02` because `dom_node_t` grew a `tag_id` field. Older
  cache files are rejected and re-parsed.

### Compatibility

- Continues to support Ruby 2.7, 3.0, 3.1, 3.2, 3.3 on Linux and
  macOS. Net::HTTP-based `Scrapetor.fetch` is unchanged so legacy
  callers don't break.
- libcurl is optional. When present at build time the gem links
  against it (+ optional libbrotlidec, libzstd, iconv); when absent
  the entire `Scrapetor::Fetcher` / `Session` surface stubs out via
  `Scrapetor::Fetcher::AVAILABLE = false` and raises a clear
  `NotAvailableError`. `SCRAP_NO_LIBCURL=1` / `SCRAP_NO_BROTLI=1` /
  `SCRAP_NO_ZSTD=1` force the stubbed path at compile.
- arm64 (Apple Silicon, AWS Graviton, Linux aarch64) gets the NEON
  path; x86_64 falls back to scalar equivalents for the SIMD
  helpers.

### Added — post-release follow-ups

- Pagination helper: `Scrapetor::Pagination.each_page(start_url) { |doc| ... }`
  with `<link rel=next>` / `a[rel~=next]` / custom-selector detection, self-loop
  guard, `:max_pages` cap, `:delay` knob.
- Form helper: `Scrapetor::Form.new(form_node, base_url:)` with default-value
  capture from every named control (text / hidden / checkbox / radio / select /
  textarea), user overrides via `form[name]=` or `merge!`, dispatch through
  GET / POST (form-encoded or multipart per `enctype`) / PUT / PATCH / DELETE.
- XPath subset: `Document#xpath` / `Node#xpath` cover descendant + child axes,
  `@attr`, `text()`, `[N]` / `[@attr]` / `[@attr='v']` / `[contains(...)]` /
  `[starts-with(...)]` / `[text()='v']`. Unsupported syntax raises
  `Scrapetor::XPath::UnsupportedError` with the offending fragment.
- XPath axes: `following-sibling::`, `preceding-sibling::`, `ancestor::`,
  `ancestor-or-self::`, plus the `comment()` node test. Sibling / ancestor
  walks dispatch to new C primitives (`node_ancestor_ids`,
  `node_following_sibling_ids`, `node_preceding_sibling_ids`); comments come
  back as `Scrapetor::CommentNode` with `text` / `content` / `comment?`.
  `//comment()` collapses to a single `node_descendant_comment_ids` C call
  via the DFS range encoding the matcher already maintains.
- Full XPath 1.0 expression engine: all 13 axes (incl. `following::`,
  `preceding::`, `attribute::`), every node test, full predicate grammar
  (`and`, `or`, `not`, comparisons, arithmetic, union), and the standard
  function library (`position`, `last`, `count`, `not`, `normalize-space`,
  `substring`, `translate`, `concat`, `contains`, `starts-with`, `string`,
  `number`, `floor`, `ceiling`, `round`, `lang`, etc.). Tokenizer + parser
  + evaluator live in `lib/scrapetor/xpath.rb`; compiled ASTs are LRU-cached.
  A translator detects CSS-compatible shapes (path with attr predicates,
  positional `[N]`, `position() > N`, `following-sibling::name`, etc.) and
  routes them through the existing native CSS chain — head-to-head benches
  against libxml-based engines clear them on common scraping idioms.
- HTTP/3 + WebSocket capability detection in `Fetcher.features`; opt into
  HTTP/3 via `http_version: "3"` when libcurl was built with it.
- mTLS / proxy auth / streaming download options on `Fetcher.get`:
  `:ssl_cert` / `:ssl_key` / `:ssl_key_password` / `:ssl_cert_type` /
  `:proxy_auth` / `:proxy_type` / `:download_to` / `:max_recv_bps` /
  `:max_send_bps`.
- Documented the limits Scrapetor doesn't try to cover (JS execution, TLS
  fingerprint impersonation) and the practical paths around them.

[0.2.0]: https://github.com/Alaa-abdulridha/scrapetor/releases/tag/v0.2.0

## [0.1.0] — Initial release

### Added

- Native C arena DOM (`ext/scrapetor/native/scrapetor_dom.c`). Single-pass
  tokeniser with class, id, and tag indexes built during parsing. Zero-copy
  text and attribute spans into the input buffer.
- Native streaming extraction engine (`ext/scrapetor/native/scrapetor_native.c`).
  Schemas compile to a flat descriptor and execute during tokenisation —
  no DOM is materialised on this path.
- Schema DSL with `field`, `repeated`, and type coercions
  (`:text`, `:integer`, `:float`, `:money`, `:date`, `:url`, `:json`,
  `:html`, `:list`, `:boolean`, `:array`).
- Field options: `clean`, `multi`, `normalize_url`, `default`, `required`,
  `transform`, `delimiter`, and array-of-fallback selectors via `from: [..]`.
- CSS selector support on the native path for tag, `.class`, `tag.class`,
  `#id`, attribute selectors with `=`, `*=`, `^=`, `$=`, `~=`, `|=`, and
  descendant + child combinators.
- Encoding detection (BOM, `<meta charset>`, `http-equiv`) with transcoding
  to UTF-8 before parsing.
- Structured-data extractors: `json_ld`, `opengraph`, `twitter_card`,
  `schema_org(type:)`, `microdata`, `rdfa`.
- Page-type detection via JSON-LD, OpenGraph, and structural signals.
- Pure-Ruby HTML builder (`Scrapetor::Builder`) and SAX streaming
  tokeniser (`Scrapetor::SAX`).
- HTTP fetcher built on `Net::HTTP` (no external gems).
- CLI binary (`scrapetor`) with `extract`, `info`, `jsonld`, `opengraph`,
  `microdata`, `rdfa`, `schema-org`, `page-type`, `encoding`, and `sax`
  subcommands.
- HTML5 named-entity decoder covering ~140 common entities plus numeric
  references.
- Plan caching (`Schema#dump`, `Schema.load`) for cross-process reuse of
  compiled extraction descriptors.
- 158 tests, four benchmark scripts comparing against Nokogiri and
  Nokolexbor.

### Compatibility

- Ruby 2.7, 3.0, 3.1, 3.2, 3.3 on Linux and macOS.
- No runtime gem dependencies.

[0.1.0]: https://github.com/Alaa-abdulridha/scrapetor/releases/tag/v0.1.0
