# frozen_string_literal: true

module Scrapetor
  # Pure-Ruby DOM. Built from the SAX tokenizer. The backing tree for
  # Scrapetor::Document when the native streaming extract path isn't
  # applicable (i.e. for `doc.css(...)`, `doc.at(...)`, mutation, and
  # serialization).
  #
  # This is intentionally minimal — node types are Element / Text /
  # Comment / Doctype, plus a Document root. The CSS selector engine
  # lives in `dom/selectors.rb`.
  module Dom
    VOID = %w[
      area base br col embed hr img input link meta source track wbr
    ].freeze

    module NodeMethods
      attr_accessor :parent

      def document
        cur = self
        cur = cur.parent while cur.respond_to?(:parent) && cur.parent
        cur
      end

      def element?; false; end
      def text?;    false; end
      def comment?; false; end
      def doctype?; false; end

      def remove
        return unless @parent
        @parent.children.delete(self)
        @parent = nil
        self
      end
      alias unlink remove
      alias delete remove

      def replace(node_or_html)
        replacements = Dom.normalize_replacement(node_or_html, parent: @parent)
        return self unless @parent
        idx = @parent.children.index(self)
        return self unless idx
        replacements.each { |r| r.parent = @parent }
        @parent.children[idx, 1] = replacements
        @parent = nil
        replacements.last
      end
      alias swap replace
      alias replace_with replace

      def add_previous_sibling(node_or_html)
        return self unless @parent
        nodes = Dom.normalize_replacement(node_or_html, parent: @parent)
        idx = @parent.children.index(self)
        return self unless idx
        nodes.each { |n| n.parent = @parent }
        @parent.children.insert(idx, *nodes)
        nodes.last
      end
      alias before add_previous_sibling

      def add_next_sibling(node_or_html)
        return self unless @parent
        nodes = Dom.normalize_replacement(node_or_html, parent: @parent)
        idx = @parent.children.index(self)
        return self unless idx
        nodes.each { |n| n.parent = @parent }
        @parent.children.insert(idx + 1, *nodes)
        nodes.last
      end
      alias after add_next_sibling

      def next_sibling
        return nil unless @parent
        sibs = @parent.children
        idx = sibs.index(self)
        idx && sibs[idx + 1]
      end

      def previous_sibling
        return nil unless @parent
        sibs = @parent.children
        idx = sibs.index(self)
        idx && idx > 0 ? sibs[idx - 1] : nil
      end

      def next_element_sibling
        cur = next_sibling
        cur = cur.next_sibling while cur && !cur.element?
        cur
      end

      def previous_element_sibling
        cur = previous_sibling
        cur = cur.previous_sibling while cur && !cur.element?
        cur
      end
    end

    class Element
      include NodeMethods

      attr_accessor :name, :attributes, :children, :line

      def initialize(name, attributes = {}, line: nil)
        @name       = name.to_s.downcase
        @attributes = attributes
        @children   = []
        @parent     = nil
        @line       = line
      end

      def element?; true; end

      # ----- attribute access -----

      def [](key)
        @attributes[key.to_s]
      end

      def []=(key, value)
        if value.nil?
          @attributes.delete(key.to_s)
        else
          @attributes[key.to_s] = value.to_s
        end
        value
      end

      def attribute_value(key)
        self[key]
      end

      def remove_attribute(key)
        @attributes.delete(key.to_s)
        self
      end

      def has_attribute?(key)
        @attributes.key?(key.to_s)
      end

      def keys
        @attributes.keys
      end

      def values
        @attributes.values
      end

      # ----- class manipulation -----

      def classes
        (self["class"] || "").split(/\s+/).reject(&:empty?)
      end

      def add_class(klass)
        set = classes
        klass.to_s.split(/\s+/).each { |c| set << c unless set.include?(c) || c.empty? }
        self["class"] = set.join(" ")
        self
      end
      alias append_class add_class

      def remove_class(klass = nil)
        if klass.nil?
          remove_attribute("class")
        else
          set = classes
          klass.to_s.split(/\s+/).each { |c| set.delete(c) }
          if set.empty?
            remove_attribute("class")
          else
            self["class"] = set.join(" ")
          end
        end
        self
      end

      def has_class?(klass)
        classes.include?(klass.to_s)
      end

      # ----- text / inner_html -----

      def text
        @children.map(&:text).join
      end
      alias content text
      alias inner_text text

      def text=(s)
        @children = [Text.new(s.to_s, parent: self)]
        s
      end
      alias content= text=

      def inner_html
        @children.map(&:to_html).join
      end

      def inner_html=(html)
        nodes = Dom::Parser.fragment(html.to_s)
        nodes.each { |n| n.parent = self }
        @children = nodes
        html
      end

      def outer_html
        attrs = serialize_attrs
        if VOID.include?(@name) && @children.empty?
          "<#{@name}#{attrs}>"
        else
          "<#{@name}#{attrs}>#{inner_html}</#{@name}>"
        end
      end
      alias to_html outer_html
      alias to_xml outer_html
      alias to_s   outer_html

      # ----- children / traversal -----

      def add_child(node_or_html)
        nodes = Dom.normalize_replacement(node_or_html, parent: self)
        nodes.each { |n| n.parent = self; @children << n }
        nodes.last
      end
      alias << add_child

      def element_children
        @children.select(&:element?)
      end
      alias elements element_children

      def first_element_child
        @children.find(&:element?)
      end

      def last_element_child
        @children.reverse_each.find(&:element?)
      end

      # ----- selectors -----

      def css(selector)
        Dom::Selectors.css(self, selector)
      end

      def at_css(selector)
        css(selector).first
      end
      alias at at_css
      alias search css

      def xpath(_expr)
        # Minimal XPath support is out of scope for the pure-Ruby DOM.
        # Callers that need full XPath can install nokogiri/nokolexbor
        # separately and pass HTML through them.
        []
      end

      def at_xpath(expr)
        xpath(expr).first
      end

      # ----- node type / misc -----

      def node_type;   1; end
      def type;        1; end
      def tag_name;    @name; end
      def node_name;   @name; end

      def path
        parts = []
        cur = self
        while cur.is_a?(Element)
          if cur["id"] && !cur["id"].empty?
            parts.unshift(cur.name + "[@id='#{cur['id']}']")
            break
          end
          idx = 1
          sib = cur.previous_sibling
          while sib
            idx += 1 if sib.is_a?(Element) && sib.name == cur.name
            sib = sib.previous_sibling
          end
          parts.unshift("#{cur.name}[#{idx}]")
          cur = cur.parent
        end
        "/" + parts.join("/")
      end

      def matches?(selector)
        document.css(selector).any? { |n| n.equal?(self) }
      end

      # Wrap this element in an HTML fragment (string) or another element,
      # placing this element as the deepest descendant of the wrapping
      # tree. Matches Nokogiri's `Node#wrap` semantics.
      def wrap(html_or_node)
        return self unless @parent
        wrapper = case html_or_node
                  when String
                    fragment = Dom::Parser.fragment(html_or_node)
                    fragment.find(&:element?) || fragment.first
                  when Element
                    html_or_node
                  else
                    Dom::Parser.fragment(html_or_node.to_s).find(&:element?)
                  end
        return self if wrapper.nil?
        # Drill to the deepest first element.
        deepest = wrapper
        while (next_level = deepest.first_element_child)
          deepest = next_level
        end
        # Replace self with the wrapper, then re-parent self under deepest.
        idx = @parent.children.index(self)
        return self unless idx
        wrapper.parent = @parent
        @parent.children[idx, 1] = [wrapper]
        @parent = deepest
        deepest.children << self
        self
      end

      def traverse(&block)
        return enum_for(:traverse) unless block_given?
        yield self
        @children.each do |c|
          if c.respond_to?(:traverse)
            c.traverse(&block)
          else
            yield c
          end
        end
        self
      end

      def attribute_nodes
        @attributes.map { |k, v| AttrNode.new(k, v, self) }
      end

      def attribute(name)
        attribute_nodes.find { |a| a.name == name.to_s }
      end

      private

      def serialize_attrs
        @attributes.map { |k, v| %( #{k}="#{Dom.escape_attr(v)}") }.join
      end
    end

    class Text
      include NodeMethods
      attr_accessor :data
      def initialize(data, parent: nil)
        @data = data.to_s
        @parent = parent
      end
      def text;     @data; end
      def content;  @data; end
      def text?;    true;  end
      def name;     "#text"; end
      def to_html;  Dom.escape_text(@data); end
      def to_s;     @data; end
      def node_type; 3; end
    end

    class Comment
      include NodeMethods
      attr_accessor :data
      def initialize(data, parent: nil)
        @data = data.to_s
        @parent = parent
      end
      def text;     ""; end
      def content;  @data; end
      def comment?; true; end
      def name;     "#comment"; end
      def to_html;  "<!--#{@data}-->"; end
      def to_s;     to_html; end
      def node_type; 8; end
    end

    class Doctype
      include NodeMethods
      attr_accessor :name
      def initialize(name, parent: nil)
        @name = name.to_s
        @parent = parent
      end
      def text;     ""; end
      def content;  ""; end
      def doctype?; true; end
      def to_html;  "<!DOCTYPE #{@name}>"; end
      def to_s;     to_html; end
      def node_type; 10; end
    end

    class AttrNode
      attr_reader :name, :value, :owner
      def initialize(name, value, owner)
        @name = name
        @value = value
        @owner = owner
      end
      def to_s;       "#{@name}=\"#{@value}\""; end
      # Nokogiri-compat: attribute nodes expose .text / .content /
      # .inner_text that return the attribute's value. Real-world code
      # iterates `node.attribute_nodes` and reads `.text` on each.
      def text;       @value.to_s; end
      alias content    text
      alias inner_text text
    end

    class Document
      include NodeMethods
      attr_accessor :doctype, :children

      def initialize
        @children = []
        @doctype  = nil
        @parent   = nil
        @class_index = nil
        @tag_index   = nil
        @id_index    = nil
      end

      def element?; false; end
      def document?; true; end
      def name; "#document"; end

      # Lazy structural indexes. Built on first access during a fallback
      # selector evaluation so the per-query candidate set drops from
      # "every element in document order" to "elements that already
      # carry the anchor class / tag / id". On a 100KB document with
      # ~5000 elements that's the difference between a 5ms walk and a
      # ~50µs lookup.
      def class_index
        @class_index ||= build_indexes![:class]
      end

      def tag_index
        @tag_index ||= build_indexes![:tag]
      end

      def id_index
        @id_index ||= build_indexes![:id]
      end

      def build_indexes!
        cls = Hash.new { |h, k| h[k] = [] }
        tag = Hash.new { |h, k| h[k] = [] }
        ids = {}
        walk = ->(node) {
          return unless node.respond_to?(:children)
          node.children.each do |c|
            next unless c.element?
            tag[c.name] << c
            id_attr = c["id"]
            ids[id_attr] ||= c if id_attr && !id_attr.empty?
            class_attr = c["class"]
            if class_attr
              class_attr.split(/\s+/).each { |t| cls[t] << c unless t.empty? }
            end
            walk.call(c)
          end
        }
        walk.call(self)
        @class_index = cls
        @tag_index = tag
        @id_index = ids
        { class: cls, tag: tag, id: ids }
      end

      def root
        @children.find(&:element?)
      end

      def html_element
        @children.find { |c| c.element? && c.name == "html" } || root
      end

      def head
        @children.flat_map { |c| c.element? ? c.css("head") : [] }.first
      end

      def body
        @children.flat_map { |c| c.element? ? c.css("body") : [] }.first
      end

      def text
        @children.map(&:text).join
      end

      def css(selector)
        Dom::Selectors.css(self, selector)
      end

      def at_css(selector)
        css(selector).first
      end
      alias at at_css

      def xpath(_expr); []; end
      def at_xpath(expr); xpath(expr).first; end

      def add_child(node_or_html)
        nodes = Dom.normalize_replacement(node_or_html, parent: self)
        nodes.each { |n| n.parent = self; @children << n }
        nodes.last
      end

      def to_html
        out = +""
        out << "<!DOCTYPE #{@doctype}>" if @doctype
        @children.each { |c| out << c.to_html }
        out
      end
      alias to_s to_html

      def traverse(&block)
        return enum_for(:traverse) unless block_given?
        yield self
        @children.each do |c|
          if c.respond_to?(:traverse)
            c.traverse(&block)
          else
            yield c
          end
        end
        self
      end
    end

    # ----- helpers -----

    def self.escape_text(s)
      s.to_s.gsub(/[&<>]/, "&" => "&amp;", "<" => "&lt;", ">" => "&gt;")
    end

    def self.escape_attr(s)
      s.to_s.gsub(/[&<>"]/,
                  "&" => "&amp;",
                  "<" => "&lt;",
                  ">" => "&gt;",
                  '"' => "&quot;")
    end

    def self.normalize_replacement(input, parent:)
      case input
      when Element, Text, Comment, Doctype then [input]
      when Array                            then input
      when String                            then Dom::Parser.fragment(input)
      else                                       [Text.new(input.to_s, parent: parent)]
      end
    end
  end
end
