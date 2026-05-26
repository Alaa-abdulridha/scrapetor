# Scrapetor

[![Gem Version](https://img.shields.io/gem/v/scrapetor.svg)](https://rubygems.org/gems/scrapetor)
[![Gem Downloads](https://img.shields.io/gem/dt/scrapetor.svg)](https://rubygems.org/gems/scrapetor)
[![CI](https://github.com/Alaa-abdulridha/scrapetor/actions/workflows/ci.yml/badge.svg)](https://github.com/Alaa-abdulridha/scrapetor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A Ruby HTML parsing and structured-extraction library. Scrapetor pairs a
native C arena DOM with a streaming extraction engine that compiles a
schema DSL into a single forward pass over the input - no DOM is
materialised, one Ruby boundary crossing per document.

The same gem also exposes a full read/mutate DOM API, encoding
detection, structured-data extractors (JSON-LD, OpenGraph, Schema.org,
Microdata, RDFa, Twitter Cards), a pure-Ruby builder and SAX streamer,
a CLI, and an HTTP fetcher built on `Net::HTTP`. There are no external
parser dependencies.

Project page: [scrapetor.org](https://scrapetor.org) ·
Source: [github.com/Alaa-abdulridha/scrapetor](https://github.com/Alaa-abdulridha/scrapetor)

## Requirements

- Ruby 2.7 or newer
- A C99-capable compiler (`clang` or `gcc`). The native extension is
  built automatically when the gem is installed.

There are no other runtime dependencies.

## Installation

Add to your `Gemfile`:

```ruby
gem "scrapetor"
```

Or install directly:

```
gem install scrapetor
```

## Quick start

```ruby
require "scrapetor"

doc = Scrapetor::HTML(html, base_url: "https://example.com/")

result = doc.extract do
  field :title, from: "h1.headline", clean: true

  repeated ".product-card", as: :products do
    field :title, from: ".title", clean: true
    field :price, from: ".price", type: :money
    field :url,   from: "a", attr: :href, type: :url, normalize_url: true
    field :image, from: "img", attr: :src, type: :url, normalize_url: true
  end
end
```

Structured-data extractors are built in:

```ruby
doc.json_ld          # Array of parsed <script type="application/ld+json"> blocks
doc.opengraph        # Hash of og:* meta values
doc.twitter_card     # Hash of twitter:* meta values
doc.schema_org(type: "Product")
doc.microdata        # HTML5 itemscope/itemprop trees
doc.page_type        # :product_page | :product_listing | :article | …
```

A minimal HTTP fetcher (uses `Net::HTTP`; no extra gems):

```ruby
doc  = Scrapetor.fetch("https://example.com/products")
data = Scrapetor.fetch_extract("https://example.com/products", schema)
```

## Production HTTP layer (libcurl, HTTP/2)

If `libcurl` is available at build time, Scrapetor ships an optional
`Scrapetor::Fetcher` backed by it. The whole pipeline — TLS,
HTTP/2 multiplexing, gzip/deflate/brotli/zstd decoding, charset
transcoding, retry, ETag cache, per-host throttle — runs in C, with
the GVL released across every network and CPU phase.

```ruby
# Single GET with HTTP/2 + connection share + retry.
resp = Scrapetor::Fetcher.get(url,
  retry: 3, backoff: 0.3, max_backoff: 10,
  bearer_token: ENV["TOKEN"],
  cache_dir: "~/.cache/scrapetor")
# => { status: 200, headers: {...}, body: "...", final_url: "...",
#      http_version: "2" }

# POST + JSON / form / multipart.
Scrapetor::Fetcher.post(url, json: {name: "alice"})
Scrapetor::Fetcher.post(url, form: {user: "x", pass: "y"})
Scrapetor::Fetcher.post(url,
  multipart: { name: "avatar",
               file: Scrapetor::Fetcher.upload_file("/tmp/pic.png") })
```

### Bulk fetch APIs

Three concurrency models pick different tradeoffs:

```ruby
# 1. pthread + easy: N workers, each blocking. Best when each
#    response has meaningful CPU work after the fetch (decode + parse)
#    since the GVL is released across the full batch.
docs = Scrapetor::Fetcher.parallel_fetch(urls, threads: 8)

# 2. curl_multi async: single driver thread, N concurrent in-flight.
#    Best for I/O-fan-out (hundreds of URLs across many hosts).
results = Scrapetor::Fetcher.multi_get(urls, max_concurrent: 32)

# 3. streaming multi_each: yields each response in completion order
#    so processing starts as soon as the first transfer lands.
Scrapetor::Fetcher.multi_each(urls) do |r|
  puts r[:final_url], r[:status]  # called as each completes
end
```

### Session: cookies, auth, throttle, retry

```ruby
session = Scrapetor::Session.new(
  cookies:     true,                 # ephemeral cookie jar (path or true)
  user_agent:  "MyBot/1.0",
  rate_limit:  0.5,                  # min seconds between same-host calls
  retry:       3,                    # default retry for all calls
  headers:     { "Accept-Language" => "en-US" },
  proxy:       ENV["HTTP_PROXY"],
)
session.post(login_url, form: {user:, pass:})
doc = session.fetch(dashboard_url)   # cookies carry forward
```

### HTTP cache with ETag / Last-Modified

```ruby
# Cold fetch: server returns 200 + ETag, response cached.
# Warm fetch: scrapetor sends If-None-Match. Server's 304 swaps in
# the cached body and marks headers["x-scrapetor-cache"] = "hit".
Scrapetor::Fetcher.get(url, cache_dir: "~/.cache/scrapetor")

# Bulk revalidation: HEAD every URL in one curl_multi sweep,
# classify each as :fresh / :changed / :missing / :error.
status = Scrapetor::Fetcher.revalidate(urls, cache_dir: "~/.cache/scrapetor")
stale  = status.select { |_, v| v == :changed }.keys
```

### Crawl helpers

```ruby
robots = Scrapetor::Robots.fetch_for("https://example.com")
robots.allowed?("https://example.com/private")  # => false
robots.crawl_delay                              # => 2.0
robots.sitemaps                                 # => [...]

Scrapetor::Sitemap.urls("https://example.com/sitemap.xml") do |url, meta|
  # streams large sitemaps without buffering in memory
  # recurses into <sitemapindex> automatically
  process(url, meta)
end
```

### Streaming HTML parser

Bounded-memory parser for huge documents:

```ruby
Scrapetor.stream(io, outer: "div.result") do |row_doc|
  # one row at a time; peak memory ~= max(chunk, longest_row)
  yield row_doc.at_css(".title").text, row_doc.at_css(".price").text
end
```

Accepts `tag`, `tag.class`, `tag.cls1.cls2`, `tag#id`, and combinations.

### Parallel parse for offline corpora

```ruby
htmls = paths.map { |p| File.read(p) }
docs  = Scrapetor.parallel_parse(htmls, threads: 8)
# Real multi-core HTML parsing under one GVL release.
```

## Limits — what Scrapetor does NOT do

Worth being explicit about what's out of scope so you can pick the
right tool for the rest of the pipeline.

### No JavaScript execution

Scrapetor reads HTML as the server sent it. Pages that build their
content client-side via React / Vue / Angular / etc. will look empty
to the parser. There's no embedded JS engine and there won't be —
that's a different class of tool (headless browser).

Practical paths if you need rendered HTML:

  - **Pre-render upstream**: many SPA hosts can pre-render for crawlers
    (`?_escaped_fragment_=`, prerender.io, Cloudflare's HTML Rewriter,
    Vercel/Netlify ISR). Cheapest if available.
  - **Headless browser layer**: drive Playwright / Puppeteer / Selenium
    from Ruby (ferrum, playwright-ruby-client). Have it spit out
    rendered HTML, then hand that to Scrapetor for fast extract.
  - **Per-site API mining**: most JS-heavy apps load data from a JSON
    API that Scrapetor can hit directly via `Fetcher.get`.

### No TLS fingerprint impersonation

Scrapetor's HTTP layer is plain libcurl. Sites that fingerprint TLS
handshakes (Cloudflare, Akamai, DataDome, Imperva) will identify the
client as libcurl and may block / challenge accordingly. Scrapetor
won't impersonate Chrome's JA3, JA4, HTTP/2 SETTINGS frame order, or
header capitalisation.

If you need impersonation:

  - **[curl-impersonate](https://github.com/lwthiker/curl-impersonate)**
    is a fork of libcurl patched to match Chrome / Firefox / Edge
    fingerprints exactly. You can build it locally and Scrapetor
    will link against it transparently — the gem's HTTP options
    are unchanged.
  - **Reach the JSON API directly** with browser-mimicking headers
    (`Accept`, `User-Agent`, `Sec-*`). Many sites only fingerprint
    the HTML route; the API is more permissive.
  - **Use a residential / mobile proxy** with a real browser at the
    other end. Scrapetor's `:proxy` + `:proxy_auth` options handle
    the proxy plumbing; the impersonation happens upstream.

The HTTP layer DOES support the rest of the production-scraping
surface: HTTP/2 multiplexing, retries with full-jitter backoff,
per-host throttle, cookie jar + auth, ETag cache, bulk revalidation,
multi-handle concurrency. Treat fingerprint impersonation as the
one externality you may need to bring yourself.

### XPath 1.0: full expression language

`Document#xpath` / `Node#xpath` evaluate the complete XPath 1.0 grammar:
all 13 axes (`child`, `descendant`, `descendant-or-self`, `parent`,
`self`, `following-sibling`, `preceding-sibling`, `following`,
`preceding`, `ancestor`, `ancestor-or-self`, `attribute`, `namespace`),
all node tests (`node()`, `text()`, `comment()`, `processing-instruction()`,
named, `*`, qualified-with-prefix), every operator (`=`, `!=`, `<`,
`<=`, `>`, `>=`, `+`, `-`, `*`, `div`, `mod`, `and`, `or`, `|`), and
the full standard function library (`not`, `last`, `position`, `count`,
`local-name`, `name`, `string`, `concat`, `starts-with`, `contains`,
`substring`, `substring-before`, `substring-after`, `string-length`,
`normalize-space`, `translate`, `boolean`, `true`, `false`, `lang`,
`number`, `sum`, `floor`, `ceiling`, `round`, `id`).

Compiled ASTs cache per unique expression string. The compiler also
detects expressions that map cleanly onto the native CSS chain
(`//div[@class='x']`, `//ul/li[1]`, `//dt/following-sibling::dd`,
`//div[position() > 50]`, etc.) and dispatches them directly to the
arena's C selector matcher — same hot path the rest of the library
rides. Sibling, ancestor, and following/preceding axis walks all run
through dedicated C primitives over the DFS-range-encoded arena, so
no Ruby per-step traversal is involved.

### HTTP/3 and WebSocket: capability-detected

`Scrapetor::Fetcher.features` reports whether the linked libcurl
exposes HTTP/3 and WebSocket support. Pass `http_version: "3"` to
opt into HTTP/3 when available; otherwise the default is HTTP/2
over TLS with HTTP/1.1 fallback. WebSocket frames go through
libcurl 7.86+'s `curl_ws_send` / `curl_ws_recv`; the gem doesn't
yet ship a friendly Ruby API for these — patches welcome.

## Command-line interface

```
$ scrapetor extract page.html --schema schema.rb
$ scrapetor info page.html
$ scrapetor jsonld page.html
$ scrapetor opengraph page.html
$ scrapetor microdata page.html
$ scrapetor page-type page.html
```

## Performance

Benchmarks live in `benchmark/comprehensive.rb`. Every workload asserts
that all three engines produce equivalent output before timing. Numbers
below were measured on Apple Silicon (Ruby 2.7.8); they're reproducible
from this repository.

**Parse throughput (build the DOM tree, MB/s).**

| Document        | Scrapetor | Nokolexbor | Nokogiri |
|-----------------|----------:|-----------:|---------:|
| small (170 B)   |        37 |         18 |       11 |
| article (2 KB)  |       279 |        116 |       31 |
| product (3 KB)  |       426 |        140 |       31 |
| listing (36 KB) |     3,933 |        154 |       31 |
| large (2.5 MB)  |    18,434 |        136 |       31 |

**CSS selector evaluation (one selector against a pre-parsed document, iter/sec).**

| Selector                                | Scrapetor | Nokolexbor |   Speedup |
|-----------------------------------------|----------:|-----------:|----------:|
| `#main` (single id)                     | 1,272,698 |     65,170 |    19.53x |
| `article` (tag)                         | 1,244,279 |     65,122 |    19.11x |
| `.product-card` (class)                 | 1,226,004 |     68,065 |    18.01x |
| `#main article` (id descendant)         | 1,086,604 |     35,228 |    30.85x |
| `img.product-image` (tag.class)         |   875,901 |     65,707 |    13.33x |
| `.product-grid > .product-card` (child) |   754,486 |     60,924 |    12.38x |
| `[data-sku="SKU0001"]` (attr)           |   516,157 |     78,101 |     6.61x |
| `.product-card .price` (descendant)     |   405,062 |     42,870 |     9.45x |

Pseudo-classes (`:has`, `:not`, `:is`, `:nth-child`, `:first-child`,
`:last-child`, `:nth-of-type`, etc.) and pseudo-elements (`::text`,
`::attr(name)`) run natively in the same C engine — see the
[Selector support](#selector-support) section below for the full list.

**End-to-end extraction (parse plus run an extraction schema, iter/sec).**

| Workload                          | Scrapetor | Nokolexbor | Nokogiri |
|-----------------------------------|----------:|-----------:|---------:|
| listing (50 cards x 4 fields)     |     9,360 |        573 |      171 |
| product detail (top + 3 reviews)  |    30,101 |     11,636 |    2,022 |
| article (top + tags + sections)   |    53,837 |     25,553 |    6,338 |

**Allocations per extraction call (live Ruby objects, lower is better).**

| Workload                         | Scrapetor | Nokolexbor | Nokogiri |
|----------------------------------|----------:|-----------:|---------:|
| listing (50 cards x 4 fields)    |       363 |      4,710 |    9,501 |
| product detail (top + 3 reviews) |        96 |        140 |      596 |

The full report - including the article workload, selector micro-benchmarks
for every supported selector form, and per-document MB/s figures - is
written to `benchmark/RESULTS.md` whenever you run `ruby -Ilib
benchmark/comprehensive.rb`.

## Architecture

```
HTML in ─┬─► Native streaming extract (C)
         │     `doc.extract(schema)`
         │     Schema runs during tokenisation. No DOM is built.
         │     One Ruby/C boundary crossing per document.
         │
         └─► Native arena DOM (C)
               `doc.css(...)`, `doc.at(...)`, mutation, traversal.
               Class/id/tag indexes built during the parse pass.
               Zero-copy text and attribute spans into the input buffer.
```

The same C extension (`ext/scrapetor/native/`) provides both paths. A
pure-Ruby DOM is kept as a fallback for environments where the
extension can't be loaded.

## Selector support

The native engine runs the following CSS forms in C:

- tag, `.class`, `tag.class`, `#id`, `tag#id`, universal `*`
- `[attr]`, `[attr=value]` and the `*=`, `^=`, `$=`, `~=`, `|=` variants
- descendant (`A B`) and child (`A > B`) combinators
- structural pseudo-classes: `:first-child`, `:last-child`, `:only-child`,
  `:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-child(...)`,
  `:nth-last-child(...)`, `:nth-of-type(...)`, `:nth-last-of-type(...)`,
  `:empty`, `:root`, `:scope`
- boolean-attribute pseudos: `:checked`, `:disabled`, `:enabled`,
  `:required`, `:optional`, `:read-only`, `:read-write`, `:any-link`,
  `:link`
- logical pseudos: `:not(...)`, `:is(...)`, `:matches(...)`, `:where(...)`,
  `:has(...)` (each runs natively when its inner selector is a single
  atom — typically a class, id, tag, or attribute predicate)
- Scrapy/Parsel-style pseudo-elements: `::text` and `::attr(name)` —
  the engine emits strings directly via a bulk C path so a 100-item
  result is one boundary crossing, not 100

Sibling combinators (`+`, `~`) and inner selectors more complex than a
single atom — for example `:not(div > .x)` or `:has(.x .y)` — transparently
fall back to a pure-Ruby implementation that mirrors Nokogiri's output.
`Selector.compile` never raises on a syntactically valid CSS selector;
the fallback is automatic.

## API reference

See the project documentation at [scrapetor.org/docs](https://scrapetor.org/docs).
The main entry points are:

- `Scrapetor.parse(html, base_url:)` - returns a `Scrapetor::Document`
- `Scrapetor::HTML(html, base_url)`  - same, Nokogiri-style alias
- `Scrapetor.schema { … }`            - schema DSL
- `Scrapetor.extract(html, schema)`   - parse and extract
- `Scrapetor.fetch(url)`              - HTTP GET and parse
- `Scrapetor.fetch_extract(url, schema)`
- `Scrapetor::Builder.build { |b| b.html { … } }`
- `Scrapetor::SAX::Parser.new(handler).parse(html)`

Document and Node expose the standard read/mutate API: `css`, `at_css`,
`xpath`, `text`, `content`, `inner_html`, `outer_html`, `[]`, `[]=`,
`attributes`, `children`, `parent`, `add_child`, `before`, `after`,
`replace`, `remove`, `add_class`, `remove_class`, and so on.

## Compatibility

Scrapetor is tested on Ruby 2.7, 3.0, 3.1, 3.2, and 3.3 on Linux and
macOS (see `.github/workflows/ci.yml`). The native extension uses only
the stable public Ruby C API.

## Development

```
git clone https://github.com/Alaa-abdulridha/scrapetor
cd scrapetor
bundle install
rake compile
rake test
```

To run the benchmarks:

```
ruby -Ilib benchmark/comprehensive.rb       # full report → benchmark/RESULTS.md
ruby -Ilib benchmark/parse_extract.rb       # listing workload only
ruby -Ilib benchmark/product_detail.rb      # product detail workload only
```

## Contributing

Issues, bug reports, and pull requests are welcome on GitHub at
<https://github.com/Alaa-abdulridha/scrapetor>. Please read
[`CONTRIBUTING.md`](CONTRIBUTING.md) before submitting a pull request.

To report a security vulnerability, follow the process in
[`SECURITY.md`](SECURITY.md).

## License

Scrapetor is released under the MIT License. See [`LICENSE`](LICENSE).
