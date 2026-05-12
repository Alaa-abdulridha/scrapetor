# Native extension

This directory contains the C source for Scrapetor's native extension.
It builds via `mkmf` and is bundled into `scrapetor_native.bundle`
(macOS) or `scrapetor_native.so` (Linux). The Ruby side loads it from
`lib/scrapetor/native.rb`.

```
ext/scrapetor/native/
├── extconf.rb           mkmf build script
├── scrapetor_native.c   streaming extraction engine
└── scrapetor_dom.c      arena DOM with class/id/tag indexes
```

## Building

The extension is built automatically when the gem is installed. For
local development:

```
rake compile
```

This produces `lib/scrapetor/scrapetor_native.{bundle,so}`. To clean:

```
rake clean
```

## Requirements

- A C99-capable compiler (`clang` on macOS, `gcc` on Linux)
- Ruby development headers (provided by your Ruby installation)

No external libraries are linked.

## Two execution paths

The extension exposes two distinct paths to Ruby:

1. **`Scrapetor::Native.extract(html, descriptor, base_url)`** —
   the streaming extraction engine. The schema descriptor is consumed
   inline with the HTML tokenisation; no DOM is materialised.
   Implemented in `scrapetor_native.c`.

2. **`Scrapetor::Native::Document.parse(html)`** — the arena DOM. One
   forward pass over the HTML builds a tree of fixed-width nodes,
   class / id / tag indexes are populated during the same pass.
   Implemented in `scrapetor_dom.c`. Exposes node-id-based accessors
   that the lazy `Scrapetor::Native::Element` Ruby wrapper consumes.

Both paths share `enc_utf8` and a handful of helpers but otherwise
operate independently.
