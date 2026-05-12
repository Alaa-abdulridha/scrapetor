# frozen_string_literal: true

module Scrapetor
  module Native
    # Wrapper module — `Scrapetor::Native::Document` is a TypedData class
    # defined in C (see ext/scrapetor/native/scrapetor_dom.c). It exposes
    # node-id based accessors. This module adds Ruby-level helpers and
    # the Element wrapper that `Scrapetor::Node` can wrap and operate on
    # the same way it does over a pure-Ruby `Dom::Element`.
    AVAILABLE_DOM = defined?(Scrapetor::Native::Document)

    if AVAILABLE_DOM
      # Lightweight wrapper: two slots, the native doc + node id.
      # Walks like a Dom::Element so the rest of Scrapetor can treat
      # it the same.
      class Element
        attr_reader :doc, :id

        def initialize(doc, id)
          @doc = doc
          @id  = id
        end

        def element?
          @doc.node_is_element(@id)
        end

        def text?;    @doc.node_type(@id) == 3; end
        def comment?; @doc.node_type(@id) == 8; end
        def document?; @doc.node_type(@id) == 9; end

        def name; @doc.node_name(@id); end
        alias node_name name
        alias tag_name name

        def [](key)
          @doc.node_attr(@id, key.to_s)
        end
        alias get_attribute []
        alias attribute_value []

        def attributes
          @doc.node_attributes(@id)
        end

        AttrNode = Struct.new(:name, :value)

        def attribute_nodes
          attributes.map { |k, v| AttrNode.new(k, v) }
        end

        def attribute(name)
          v = self[name]
          v && AttrNode.new(name.to_s, v)
        end

        def keys;    attributes.keys; end
        def values;  attributes.values; end
        def has_attribute?(k); !@doc.node_attr(@id, k.to_s).nil?; end
        alias key? has_attribute?

        def path
          parts = []
          cur = self
          while cur && cur.element?
            id = cur["id"]
            if id && !id.empty?
              parts.unshift("#{cur.name}[@id='#{id}']")
              break
            end
            idx = 1
            sib = cur.previous_sibling
            while sib
              if sib.element? && sib.name == cur.name
                idx += 1
              end
              sib = sib.previous_sibling
            end
            parts.unshift("#{cur.name}[#{idx}]")
            cur = cur.parent
          end
          "/" + parts.join("/")
        end

        def fragment?; false; end
        def cdata?;    false; end
        def processing_instruction?; false; end

        def text
          @doc.node_text(@id)
        end
        alias content text
        alias inner_text text

        def parent
          pid = @doc.node_parent(@id)
          pid ? Element.new(@doc, pid) : nil
        end

        def children
          @doc.node_children(@id).map { |cid| Element.new(@doc, cid) }
        end

        def element_children
          @doc.node_element_children(@id).map { |cid| Element.new(@doc, cid) }
        end
        alias elements element_children

        def first_element_child
          ids = @doc.node_element_children(@id)
          ids.empty? ? nil : Element.new(@doc, ids.first)
        end

        def last_element_child
          ids = @doc.node_element_children(@id)
          ids.empty? ? nil : Element.new(@doc, ids.last)
        end

        def next_sibling
          nid = @doc.node_next_sibling(@id)
          nid ? Element.new(@doc, nid) : nil
        end

        def previous_sibling
          nid = @doc.node_prev_sibling(@id)
          nid ? Element.new(@doc, nid) : nil
        end

        def next_element_sibling
          cur = @doc.node_next_sibling(@id)
          while cur && !@doc.node_is_element(cur)
            cur = @doc.node_next_sibling(cur)
          end
          cur ? Element.new(@doc, cur) : nil
        end

        def previous_element_sibling
          cur = @doc.node_prev_sibling(@id)
          while cur && !@doc.node_is_element(cur)
            cur = @doc.node_prev_sibling(cur)
          end
          cur ? Element.new(@doc, cur) : nil
        end

        def classes
          @doc.node_classes(@id)
        end

        def has_class?(klass); classes.include?(klass.to_s); end

        def css(selector)
          all = []
          seen = {}
          Native.split_selector_groups(selector.to_s).each do |g|
            plan = Native.compile_selector_chain(g)
            @doc.run_chain(plan, @id).each do |nid|
              next if seen[nid]
              seen[nid] = true
              all << Element.new(@doc, nid)
            end
          end
          all
        end

        def at_css(selector)
          Native.split_selector_groups(selector.to_s).each do |g|
            plan = Native.compile_selector_chain(g)
            ids = @doc.run_chain(plan, @id)
            return Element.new(@doc, ids.first) unless ids.empty?
          end
          nil
        end
        alias at at_css
        alias search css

        def xpath(_expr);    []; end
        def at_xpath(_expr); nil; end

        def inner_html
          element_children.map(&:to_html).join + text_only_children
        end

        def outer_html
          attr_str = attributes.map { |k, v| %( #{k}="#{Dom.escape_attr(v)}") }.join
          if Dom::VOID.include?(name) && @doc.node_children(@id).empty?
            "<#{name}#{attr_str}>"
          else
            "<#{name}#{attr_str}>#{inner_html}</#{name}>"
          end
        end
        alias to_html outer_html
        alias to_xml outer_html
        alias to_s outer_html

        def node_type; @doc.node_type(@id); end
        alias type node_type

        def ==(other)
          other.is_a?(Element) && @doc.equal?(other.doc) && @id == other.id
        end
        alias eql? ==

        def hash
          [@doc.object_id, @id].hash
        end

        def fingerprint
          Scrapetor::Fingerprint.structural(self)
        end

        private

        def text_only_children
          # Concatenate text children's raw text (text nodes that aren't
          # part of an element subtree).
          children = @doc.node_children(@id)
          children.filter_map do |cid|
            @doc.node_type(cid) == 3 ? @doc.node_text(cid) : nil
          end.join
        end

        def compile(selector)
          Native.compile_selector_chain(selector)
        end
      end

      # Document wrapper — wraps Native::Document and provides Dom-like
      # methods so `Scrapetor::Document#backing` can return one of these
      # interchangeably with `Dom::Document`.
      class DocumentWrapper
        attr_reader :native

        def initialize(native)
          @native = native
        end

        def element?; false; end
        def document?; true; end
        def name; "#document"; end

        def root
          rid = @native.root_id
          Element.new(@native, rid)
        end

        def root_element; root; end

        def text;     ""; end
        def content;  ""; end

        def css(selector)
          all = []
          seen = {}
          Native.split_selector_groups(selector.to_s).each do |g|
            plan = Native.compile_selector_chain(g)
            @native.run_chain(plan, nil).each do |id|
              next if seen[id]
              seen[id] = true
              all << Element.new(@native, id)
            end
          end
          all
        end

        def at_css(selector)
          Native.split_selector_groups(selector.to_s).each do |g|
            plan = Native.compile_selector_chain(g)
            ids = @native.run_chain(plan, nil)
            return Element.new(@native, ids.first) unless ids.empty?
          end
          nil
        end
        alias at at_css

        def xpath(_expr); []; end
        def at_xpath(_expr); nil; end

        def to_html
          @native.html
        end
        alias to_s to_html

        def html
          root
        end

        def body
          at_css("body")
        end

        def head
          at_css("head")
        end
      end
    end  # if AVAILABLE_DOM

    # ----- selector compilation: CSS string -> chain of native plans -----

    # Each plan entry is `[selector_atom, combinator_or_nil]`.
    # selector_atom = [tag_or_nil, classes_array, id_or_nil, attrs_array]
    # combinator = nil | "descendant" | "child"
    def self.compile_selector_chain(selector_str)
      plan = Scrapetor::Selector.compile(selector_str)
      plan.map do |atom|
        sel = [
          atom.tag ? atom.tag.to_s : nil,
          atom.classes,
          atom.id,
          atom.attrs
        ]
        combo =
          case atom.combinator
          when :descendant then "descendant"
          when :child      then "child"
          else nil
          end
        [sel, combo]
      end
    end

    # Split a CSS selector on top-level commas (outside [...]).
    def self.split_selector_groups(s)
      groups = []
      buf = +""
      depth = 0
      s.each_char do |ch|
        case ch
        when "[" then depth += 1; buf << ch
        when "]" then depth -= 1 if depth.positive?; buf << ch
        when ","
          if depth.zero?
            groups << buf.strip
            buf = +""
          else
            buf << ch
          end
        else
          buf << ch
        end
      end
      groups << buf.strip
      groups.reject(&:empty?)
    end
  end
end
