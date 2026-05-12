# Changelog

All notable changes to Scrapetor are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/).

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
