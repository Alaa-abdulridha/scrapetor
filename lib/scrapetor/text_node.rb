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

    def inspect
      "#<Scrapetor::TextNode #{super}>"
    end
  end
end
