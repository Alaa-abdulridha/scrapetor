# frozen_string_literal: true

module Scrapetor
  # Result type for `::text` and `::attr(name)` pseudo-element queries.
  #
  # Scrapy / Parsel-style code expects strings directly from these
  # selectors (`doc.css("h3::text").get`), but Nokogiri-style scrapers
  # routinely chain a `.text` / `.content` accessor onto each result
  # (`doc.css("h3::text").first.text` or `node.at("a::attr(href)").text`).
  # Returning a bare String breaks the Nokogiri-style call path with
  # NoMethodError, even though the String already _is_ the text we
  # would have returned.
  #
  # TextNode is a thin String subclass that closes the gap: it equals,
  # compares, splits, and concatenates exactly like a String, and adds
  # the Node-shaped accessors (`text`, `content`, `inner_text`, `name`,
  # `element?`, `text?`) plus the Parsel-shaped `get` / `getall`. The
  # underlying byte string is the actual text content; the extra methods
  # all return self (or trivial derivatives), so chaining stays cheap.
  class TextNode < String
    def text;       String.new(self); end
    alias inner_text text
    alias content    text

    # Parsel-style accessors.
    def get;        String.new(self); end
    def getall;     [String.new(self)]; end

    # Node-shape predicates so duck-typing checks (`n.element?`,
    # `n.text?`, `n.name == "#text"`) don't blow up.
    def name;       "#text"; end
    def element?;   false; end
    def text?;      true; end
    def comment?;   false; end
    def document?;  false; end
    def cdata?;     false; end

    def to_html;    self.to_s; end
    alias outer_html to_html
    alias inner_html to_html

    # No-op mutation API. Heterogeneous selectors like
    # `.foo > ::text, .bar` can hand a TextNode to a caller that
    # assumes an Element interface (e.g.
    # `node.inner_html = node.inner_html.gsub(...)`). The reassignment
    # would crash on bare String; we accept the write silently so the
    # subsequent `.text` read still works. The mutation is intentionally
    # dropped — TextNode wraps frozen content of the original element.
    def inner_html=(_v); _v; end
    def content=(_v);    _v; end
    def []=(*_args);     nil; end
    def add_class(_k);    self; end
    def remove_class(*_); self; end
    def remove;           self; end
    def unlink;           self; end

    # Containing element (the node whose text/attribute this TextNode
    # represents). Set by the css() boundary when we know the parent;
    # left nil otherwise. Production code chains
    # `result.at(::text).parent.css(...)` to navigate to siblings of
    # the text node, mirroring the Nokogiri shape where text nodes
    # carry a `.parent` back-reference.
    attr_accessor :parent_node

    def parent;                 @parent_node; end
    def next_sibling;           nil; end
    def previous_sibling;       nil; end
    def next_element_sibling;   nil; end
    def previous_element_sibling; nil; end
    def children;               []; end
    def element_children;       []; end
    def attributes;             {}; end
    def attribute_nodes;        []; end
    def attribute(_name);       nil; end
    def keys;                   []; end
    def values;                 []; end
    def classes;                []; end
    def has_class?(_klass);     false; end
    def [](*args)
      # String byte/range subscript when called with a single non-string
      # argument; nil for attribute-style String access.
      if args.size == 1 && args.first.is_a?(String)
        nil
      elsif args.size == 1 && args.first.is_a?(Symbol)
        nil
      else
        super
      end
    end
    def css(_selector);         []; end
    def at_css(_selector);      nil; end
    def at(_selector);          nil; end
    def search(_selector);      []; end
    def xpath(*_args);          []; end
    def at_xpath(*_args);       nil; end

    def inspect
      "#<Scrapetor::TextNode #{super}>"
    end
  end
end
