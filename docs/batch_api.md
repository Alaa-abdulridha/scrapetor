# Batch / extraction API

Scrapetor exposes a declarative extraction layer that collapses the
common scrape pattern — "for every match of an outer selector, build a
hash from inner field selectors" — into a single Ruby↔C round-trip
instead of one per `at_css` / `text` / `[attr]` call.

## Why

A typical SERP-style parser walks ~50 result blocks and pulls ~5
fields from each. That's ~250 individual `at_css` calls per page, and
each call pays the Ruby method-dispatch cost plus a fresh `Element` /
`Node` allocation. Even when every selector is already in the compile
cache and every result is in the per-document result cache, the
dispatch overhead alone runs ~3–5 µs per call → ~1 ms per page of
pure plumbing tax.

The batch API moves the loop into C. One call, one C-side iteration,
one Hash per match assembled directly from a shared simple-atom pool.

## Methods

### `Element#batch_css(selectors)` / `Node#batch_css(selectors)`

Take an Array of selector strings, return parallel results in one C
call.

```ruby
title_ns, price_strs, hrefs = result.batch_css([
  ".title",
  ".price::text",
  "a::attr(href)",
])
```

Each result is whatever the corresponding `css(...)` would have
returned: a `NodeSet` for plain selectors, an Array of strings for
`::text` / `::attr(...)`.

### `Element#extract(map)` / `Node#extract(map)`

Single-result form. Each value is the first match for its selector,
scoped to the receiver.

```ruby
row = result.extract(
  title: ".title::text",
  price: ".price::text",
  href:  "a::attr(href)",
)
# => { title: TextNode, price: TextNode, href: TextNode }
```

### `Element#extract_each(outer, fields)` / `Node#extract_each(...)`

Iterate matches of `outer` under the receiver and produce a Hash per
match from the inner field selectors.

```ruby
container.extract_each(".result", title: ".t::text", price: ".p::text")
# => [{ title: ..., price: ... }, ...]
```

### `Document#extract_each(outer, fields)`

Document-scope iterate-and-extract. **This one runs entirely inside a
single C call** when every selector compiles natively — the outer
plan + all inner plans + every match × field tuple, with zero Ruby↔C
round-trips on the hot path.

```ruby
rows = doc.extract_each(".result", {
  title: ".title::text",
  price: ".price::text",
  href:  "a::attr(href)",
})
```

### `NodeSet#extract(fields)`

Maps the helper across every node in the set so the standard chain
shape works:

```ruby
doc.css(".result").extract(title: ".t::text", price: ".p::text")
```

### `Document#batch_css(selectors)` / `Document#extract_css(map)`

Document-level batch (pre-existing). Useful when you want multiple
disjoint top-level selectors:

```ruby
data = doc.extract_css(
  organic_results: ".organic .result",
  ad_results:      ".ad-card",
  related_terms:   ".related-search-term::text",
)
```

## Performance

On a synthetic 50-result page extracting 3 fields per result over
1000 iterations:

| Pattern                           | Per-op  |
|-----------------------------------|---------|
| Individual `node.at_css(...)` loop| 122 µs  |
| `doc.extract_each(...)`           |  44 µs  |
| **Speedup**                       | **2.76×** |

The speedup is fully attributable to eliminated per-call Ruby
dispatch. Heavier workloads with more fields per match see larger
relative gains.

## Pseudo-element conventions

- `selector::text` — concatenated subtree text of the first match.
  Returned as `Scrapetor::TextNode` (a String subclass).
- `selector::attr(name)` — value of `name` on the first match.
  Returned as `Scrapetor::TextNode`.
- `::attr(name)` standalone — value of `name` on the outer match
  itself (no inner traversal needed).
- Plain selector — first matching Element wrapped in a Node.

## Compatibility

The batch path is enabled automatically when:

1. The document is backed by the native arena (i.e. produced by
   `Scrapetor.parse`).
2. Every selector compiles cleanly via `compile_selector_chain`.

If a selector compiles to `nil` (rare; the engine handles every CSS
Selectors Level 4 shape audited against production parsers, including
multi-chain `:has(X Y, A B)` and recursive `:has(.x:not(.y))`), the
implementation falls back to the per-row Ruby loop transparently.
Results are identical in either path.
