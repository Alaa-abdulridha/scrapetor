# Scrapetor

[![Gem Version](https://img.shields.io/gem/v/scrapetor.svg)](https://rubygems.org/gems/scrapetor)
[![CI](https://github.com/Alaa-abdulridha/scrapetor/actions/workflows/ci.yml/badge.svg)](https://github.com/Alaa-abdulridha/scrapetor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A Ruby HTML parsing and structured-extraction library. Scrapetor pairs a
native C arena DOM with a streaming extraction engine that compiles a
schema DSL into a single forward pass over the input — no DOM is
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

**Parse — build the DOM tree.**

| Document        | Scrapetor (MB/s) | Nokolexbor (MB/s) | Nokogiri (MB/s) |
|-----------------|-----------------:|------------------:|----------------:|
| small (170 B)   |              35  |               17  |             11  |
| product (3 KB)  |             230  |              136  |             29  |
| listing (36 KB) |             381  |              152  |             30  |
| large (2.5 MB)  |             341  |              132  |             29  |

**CSS selector evaluation — one selector against a pre-parsed document.**

| Selector                        | Scrapetor i/s | Nokolexbor i/s |
|---------------------------------|--------------:|---------------:|
| `#main` (single id)             |       309 610 |         64 570 |
| `article` (tag)                 |        72 312 |         60 790 |
| `.product-card` (class)         |        66 104 |         63 891 |
| `.product-card .price`          |        43 466 |         42 699 |

**End-to-end extraction — parse plus run an extraction schema.**

| Workload                          | Scrapetor i/s | Nokolexbor i/s | Nokogiri i/s |
|-----------------------------------|--------------:|---------------:|-------------:|
| listing (50 cards × 4 fields)     |         8 356 |            550 |          169 |
| product detail (top + 3 reviews)  |        17 913 |         11 563 |        1 986 |
| article (top + tags + sections)   |        25 299 |         27 372 |        6 302 |

**Allocations — live Ruby objects per extraction call.**

| Workload                         | Scrapetor | Nokolexbor | Nokogiri |
|----------------------------------|----------:|-----------:|---------:|
| listing (50 cards × 4 fields)    |       449 |      4 710 |    9 501 |
| product detail (top + 3 reviews) |       262 |        140 |      596 |

The full report — including the article workload, selector micro-benchmarks
for every supported selector form, and per-document MB/s figures — is
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

The native engine supports:

- tag, `.class`, `tag.class`, `#id`, `tag#id`
- `[attr]`, `[attr=value]`, and the `*=`, `^=`, `$=`, `~=`, `|=` variants
- descendant (`A B`) and child (`A > B`) combinators

Selectors that fall outside this subset transparently fall back to a
Ruby reference implementation, which still matches Nokogiri's output.

## API reference

See the project documentation at [scrapetor.org/docs](https://scrapetor.org/docs).
The main entry points are:

- `Scrapetor.parse(html, base_url:)` — returns a `Scrapetor::Document`
- `Scrapetor::HTML(html, base_url)`  — same, Nokogiri-style alias
- `Scrapetor.schema { … }`            — schema DSL
- `Scrapetor.extract(html, schema)`   — parse and extract
- `Scrapetor.fetch(url)`              — HTTP GET and parse
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
