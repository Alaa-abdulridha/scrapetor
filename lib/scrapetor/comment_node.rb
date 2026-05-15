# frozen_string_literal: true

module Scrapetor
  # Result type for XPath `comment()` queries (`//comment()`,
  # `child::comment()`, etc.). Carries the comment's text payload and
  # implements the Node-shape predicate methods so duck-typing checks
  # (`n.comment?`, `n.element?`, `n.name == "#comment"`) match what
  # Nokogiri would return.
  #
  # The constructor accepts a String (extracted by the native engine's
  # `node_comment_text`) or a Dom::Comment (Ruby fallback path); in
  # either case `#text` / `#content` returns the payload between
  # `<!--` and `-->`.
  class CommentNode
    attr_reader :document

    def initialize(document, payload)
      @document = document
      @text = payload.is_a?(String) ? payload :
              (payload.respond_to?(:content) ? payload.content.to_s : payload.to_s)
    end

    def text;        @text; end
    alias content     text
    alias inner_text  text

    def to_s;        @text; end
    def to_html;     "<!--#{@text}-->"; end
    alias outer_html to_html
    alias inner_html to_html

    def name;        "#comment"; end
    alias node_name  name

    def comment?;    true;  end
    def element?;    false; end
    def text?;       false; end
    def document?;   false; end
    def cdata?;      false; end
    def node_type;   8;     end

    # Node-shape probes that scraping code occasionally fires against
    # mixed result sets. Returning a benign default keeps a stray
    # `.css(...)` or `.attributes` from raising NoMethodError when a
    # caller iterates over an Array<Element + CommentNode>.
    def attributes;  {};    end
    def attribute_nodes; []; end
    def attribute(_); nil; end
    def keys;        [];    end
    def values;      [];    end
    def children;    [];    end
    def element_children; []; end
    def classes;     [];    end
    def has_class?(_); false; end
    def [](*_args);  nil;   end
    def css(_);      [];    end
    def at_css(_);   nil;   end
    def at(_);       nil;   end
    def search(_);   [];    end
    def xpath(*_);   [];    end
    def at_xpath(*_); nil;  end

    def inspect
      "#<Scrapetor::CommentNode #{@text.inspect}>"
    end
  end
end
