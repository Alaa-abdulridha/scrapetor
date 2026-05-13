# frozen_string_literal: true

require "date"

module Scrapetor
  # Featherweight node wrapper. Holds a document reference and a backing
  # Nokolexbor element. Selector ops delegate to the backing engine in
  # Phase 1; the native extension (Phase 2) replaces this with arena-DOM
  # + bytecode VM.
  class Node
    def initialize(doc, backing)
      @doc = doc
      @nlx = backing
    end

    def text
      @nlx.text
    end

    def clean_text
      Cleaner.clean(text)
    end

    def visible_text
      stripped = @nlx.dup
      stripped.css("script, style, noscript").each(&:remove) if stripped.respond_to?(:css)
      Cleaner.clean(stripped.text)
    end

    def inner_html
      @nlx.inner_html
    end

    def outer_html
      @nlx.to_html
    end
    alias to_html outer_html

    def name
      @nlx.name
    end
    alias node_name name
    alias tag_name  name

    # Nokogiri-compat: `content` and `inner_text` are aliases for `text`.
    alias content    text
    alias inner_text text

    # Nokogiri-compat: return all element attributes as a Hash.
    def attributes
      h = {}
      @nlx.attribute_nodes.each { |a| h[a.name] = a.value }
      h
    end

    def keys
      @nlx.attribute_nodes.map(&:name)
    end

    def values
      @nlx.attribute_nodes.map(&:value)
    end

    def has_attribute?(name)
      !@nlx[name.to_s].nil?
    end
    alias key? has_attribute?
    alias attribute? has_attribute?

    def element?
      @nlx.respond_to?(:element?) ? @nlx.element? : true
    end

    def document?; false; end

    # Iterate over attributes as Nokogiri does.
    def each_attribute
      return enum_for(:each_attribute) unless block_given?
      @nlx.attribute_nodes.each { |a| yield [a.name, a.value] }
    end

    def attr(key)
      @nlx[key.to_s]
    end

    def [](key)
      @nlx[key.to_s]
    end

    def absolute_url(base = nil)
      href = @nlx["href"] || @nlx["src"]
      URL.absolute(href, base || @doc.base_url)
    end

    def money
      Money.parse(text)
    end

    def number
      v = text.to_s.gsub(/[^\d.\-]/, "")
      return nil if v.empty? || v == "-"
      v.include?(".") ? v.to_f : v.to_i
    end

    def date
      Date.parse(text.to_s)
    rescue ArgumentError, TypeError
      nil
    end

    # Nokogiri-compat: `node.css(selector, ns_or_handler)`. Extra args
    # are XPath-only and harmless to ignore for CSS.
    def css(selector, *_extra)
      result = @nlx.css(selector)
      # `::text` / `::attr(name)` queries hand back a flat Array of
      # String/TextNode. Pass that through as-is. For everything else
      # — including the empty-NodeSet case — wrap in a NodeSet so the
      # caller can chain `.at_css`, `.each_with_index`, etc. Detect the
      # pseudo-element shape by checking the selector string; relying
      # on the result shape would mis-classify zero-match queries.
      if result.is_a?(Array) && selector_pseudo_element?(selector)
        return result
      end
      NodeSet.new(@doc, result.to_a)
    end

    def at(selector, *_extra)
      n = @nlx.at_css(selector)
      return n if n.is_a?(String)
      n && Node.new(@doc, n)
    end

    private

    def selector_pseudo_element?(sel)
      s = sel.to_s
      s.include?("::") && s =~ /::(?:text|attr\([^)]+\)|first-letter|first-line|before|after)\s*\z/i
    end

    public
    alias at_css at
    alias search css

    # Native C versions of Node#at and Node#css are installed by
    # native_dom.rb after the Native extension module is loaded —
    # they aren't available at this point in the require chain.

    # Batch API: array of selector strings → array of results,
    # one C round-trip total. Delegates to the underlying Element's
    # batch_css; falls back to N individual css() calls if the
    # backing node doesn't expose batch.
    def batch_css(selectors)
      if @nlx.respond_to?(:batch_css)
        results = @nlx.batch_css(selectors)
        results.map do |r|
          case r
          when Array
            # ::text / ::attr results — array of strings; pass through.
            # Element arrays — wrap in NodeSet.
            if r.empty? || r.first.is_a?(String)
              r
            else
              NodeSet.new(@doc, r)
            end
          else
            r # NodeSet or other
          end
        end
      else
        selectors.map { |s| css(s) }
      end
    end

    # Hash-form batch: {key => selector} → {key => result}.
    def extract_css(map)
      keys = map.keys
      results = batch_css(map.values)
      out = {}
      keys.each_with_index { |k, i| out[k] = results[i] }
      out
    end

    # Per-result extract: at_css for each field. Returns Hash.
    def extract(map)
      out = {}
      map.each_pair { |k, sel| out[k] = at_css(sel) }
      out
    end

    # Iterate matches under this node, build a Hash from `fields` for
    # each. Mirrors the SerpApi-style result-loop pattern as one
    # declarative call:
    #
    #   node.extract_each(".item", title: ".t::text", price: ".p::text")
    #   # => [{title: "...", price: "..."}, ...]
    def extract_each(outer_selector, fields)
      css(outer_selector).map { |n| n.extract(fields) }
    end

    def children
      kids = @nlx.children.to_a.select { |c| c.respond_to?(:element?) && c.element? }
      NodeSet.new(@doc, kids)
    end

    def parent
      p = @nlx.parent
      return nil if p.nil? || (defined?(Dom::Document) && p.is_a?(Dom::Document))
      Node.new(@doc, p)
    end

    # Nokogiri-compatible: returns the literal next node (may be a text /
    # comment node). Use `next_element_sibling` (or `next_element`) to skip
    # non-element siblings.
    def next_sibling
      sib = @nlx.next_sibling
      sib && Node.new(@doc, sib)
    end

    def previous_sibling
      sib = @nlx.previous_sibling
      sib && Node.new(@doc, sib)
    end

    def next_element_sibling
      sib = @nlx.next_sibling
      while sib && !(sib.respond_to?(:element?) && sib.element?)
        sib = sib.next_sibling
      end
      sib && Node.new(@doc, sib)
    end
    alias next_element next_element_sibling

    def previous_element_sibling
      sib = @nlx.previous_sibling
      while sib && !(sib.respond_to?(:element?) && sib.element?)
        sib = sib.previous_sibling
      end
      sib && Node.new(@doc, sib)
    end
    alias previous_element previous_element_sibling

    def fingerprint
      Fingerprint.structural(self)
    end

    def backing_node
      @nlx
    end

    # ----- Mutation API (delegated to Nokolexbor) -----

    def []=(key, value)
      @nlx[key.to_s] = value.nil? ? nil : value.to_s
      value
    end
    alias set_attribute []=

    def get_attribute(key)
      @nlx[key.to_s]
    end

    def remove_attribute(key)
      @nlx.remove_attribute(key.to_s)
      self
    end
    alias delete_attribute remove_attribute

    def content=(text)
      @nlx.content = text.to_s
      text
    end

    def inner_html=(html)
      @nlx.inner_html = html.to_s
      html
    end

    def add_child(node_or_html)
      wrap_result(@nlx.add_child(unwrap_mut(node_or_html)))
    end
    alias << add_child
    alias add_child! add_child

    def add_previous_sibling(node_or_html)
      wrap_result(@nlx.add_previous_sibling(unwrap_mut(node_or_html)))
    end
    alias before add_previous_sibling

    def add_next_sibling(node_or_html)
      wrap_result(@nlx.add_next_sibling(unwrap_mut(node_or_html)))
    end
    alias after add_next_sibling

    def replace(node_or_html)
      wrap_result(@nlx.replace(unwrap_mut(node_or_html)))
    end
    alias replace_with replace
    alias swap        replace

    def remove
      @nlx.remove
      self
    end
    alias unlink remove
    alias delete remove

    # ----- Class manipulation -----

    def add_class(klass)
      @nlx.add_class(klass.to_s)
      self
    end
    alias append_class add_class

    def remove_class(klass = nil)
      if klass.nil?
        @nlx.remove_attribute("class")
      else
        @nlx.remove_class(klass.to_s)
      end
      self
    end

    def classes
      (@nlx["class"] || "").split(/\s+/).reject(&:empty?)
    end

    def has_class?(klass)
      classes.include?(klass.to_s)
    end

    # ----- Extra Nokogiri-compat aliases -----

    alias prev     previous_sibling
    alias previous previous_sibling
    alias next     next_sibling

    def first_element_child
      c = @nlx.children.to_a.find { |x| x.respond_to?(:element?) && x.element? }
      c && Node.new(@doc, c)
    end

    # Nokogiri-compat: `Node#child` returns the first child regardless
    # of node type (text / element / comment). Used by parsers that
    # poke at the immediate inner content (e.g. heading nodes whose
    # text lives in a text-node child).
    def child
      c = @nlx.children.to_a.first
      c && Node.new(@doc, c)
    end

    def last_element_child
      c = @nlx.children.to_a.reverse.find { |x| x.respond_to?(:element?) && x.element? }
      c && Node.new(@doc, c)
    end

    def element_children
      kids = @nlx.children.to_a.select { |x| x.respond_to?(:element?) && x.element? }
      NodeSet.new(@doc, kids)
    end
    alias elements element_children

    def node_type
      @nlx.respond_to?(:node_type) ? @nlx.node_type : 1
    end
    alias type node_type

    def path
      @nlx.path if @nlx.respond_to?(:path)
    end

    # Build a minimal CSS path back to this node (id-based when
    # available, falling back to tag + :nth-of-type indexing).
    def css_path
      parts = []
      cur = @nlx
      while cur && cur.respond_to?(:name) && cur.element?
        if (id = cur["id"]) && !id.empty?
          parts.unshift("##{id}")
          break
        end
        index = 1
        sib = cur.previous_sibling
        while sib
          index += 1 if sib.respond_to?(:element?) && sib.element? && sib.name == cur.name
          sib = sib.previous_sibling
        end
        parts.unshift("#{cur.name}:nth-of-type(#{index})")
        cur = cur.parent
      end
      parts.join(" > ")
    end

    # XPath path to this node.
    def xpath_path
      path
    end

    def traverse(&block)
      return enum_for(:traverse) unless block_given?
      yield self
      element_children.each { |c| c.traverse(&block) }
    end

    def ancestors(selector = nil)
      list = []
      cur = parent
      while cur
        list << cur
        cur = cur.parent
      end
      result = NodeSet.new(@doc, list.map(&:backing_node))
      selector.nil? ? result : result.select { |n| n.matches?(selector) }
    end

    def matches?(selector)
      ns = @doc.css(selector)
      ns.to_a.any? { |n| n.backing_node == @nlx }
    end

    # XPath helpers. The native engine doesn't yet implement XPath, so we
    # return empty results rather than NoMethodError on Node — this keeps
    # callers that probe both engines from crashing.
    def xpath(*_exprs)
      Scrapetor::NodeSet.new(@doc, [])
    end

    def at_xpath(*_exprs)
      nil
    end

    def wrap(html_or_node)
      if @nlx.respond_to?(:wrap)
        @nlx.wrap(html_or_node)
      end
      self
    end

    def blank?
      text.to_s.strip.empty?
    end

    def attribute_nodes
      @nlx.attribute_nodes
    end

    def attribute(name)
      @nlx.attribute_nodes.find { |a| a.name == name.to_s }
    end

    def to_xml(*args)
      @nlx.to_html(*args)
    end
    alias to_str to_html

    def comment?
      node_type == 8
    end

    def text?
      node_type == 3
    end
    alias text_node? text?

    def cdata?
      node_type == 4
    end

    def processing_instruction?
      node_type == 7
    end

    def fragment?
      false
    end

    def document
      @doc
    end

    def root
      @doc.root
    end

    def write_to(io, *args)
      io.write(to_html(*args))
    end

    def serialize(*args)
      to_html(*args)
    end

    def ==(other)
      other.is_a?(Node) && @nlx == other.backing_node
    end
    alias eql? ==

    def hash
      @nlx.hash
    end

    private

    def wrap_result(result)
      return nil if result.nil?
      case result
      when Node
        result
      when Array
        NodeSet.new(@doc, result)
      else
        if result.respond_to?(:element?) && result.element?
          Node.new(@doc, result)
        else
          result
        end
      end
    rescue StandardError
      result
    end

    def unwrap_mut(node_or_html)
      if node_or_html.is_a?(Node)
        node_or_html.backing_node
      else
        node_or_html
      end
    end
  end
end
