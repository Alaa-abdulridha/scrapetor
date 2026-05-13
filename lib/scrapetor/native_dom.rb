# frozen_string_literal: true

module Scrapetor
  module Native
    # Wrapper module — `Scrapetor::Native::Document` is a TypedData class
    # defined in C (see ext/scrapetor/native/scrapetor_dom.c). It exposes
    # node-id based accessors. This module adds Ruby-level helpers and
    # the Element wrapper that `Scrapetor::Node` can wrap and operate on
    # the same way it does over a pure-Ruby `Dom::Element`.
    AVAILABLE_DOM = defined?(Scrapetor::Native::Document)

    # ----- pseudo-element handling at the css() boundary -----

    PSEUDO_ELEMENT_RE = /(::(?:text|attr\([^)]+\)|first-letter|first-line|before|after))\s*\z/i.freeze

    # Wrap each String entry in TextNode so Node-style `.text` /
    # `.content` accessors and Parsel-style `.get` / `.getall` both work.
    # Skips nil (`bulk_attr` returns nil for missing attributes) and any
    # value that's already a TextNode. Mutates in place to avoid a second
    # Array allocation on the result-collection hot path.
    def self.wrap_text_nodes!(arr)
      return arr unless arr.is_a?(Array)
      i = 0
      n = arr.length
      while i < n
        v = arr[i]
        arr[i] = Scrapetor::TextNode.new(v) if v.is_a?(String) && !v.is_a?(Scrapetor::TextNode)
        i += 1
      end
      arr
    end

    # `::text` and `::attr(name)` are Scrapy/Parsel-style pseudo-elements:
    # they reshape the result of a selector into strings rather than
    # affecting matching. Strip them before running the query and apply
    # the transform on the way out.
    #
    # Returns [stripped_selector, transform_kind, arg]
    #   transform_kind = nil | :text | :attr | :text_approx
    #
    # Fast-path skip when the selector has no `::` substring (the common
    # case) — saves a regex match on every css() call.
    def self.peel_pseudo_element(selector_str)
      s = selector_str
      return [s, nil, nil] unless s.include?("::")
      m = s.match(PSEUDO_ELEMENT_RE)
      return [s, nil, nil] unless m
      head = s[0...m.begin(0)].rstrip
      pe = m[1]
      # `head > ::text` and `head > ::attr(x)`: strip the trailing `>`
      # combinator and flip kind into the direct-only variant. The
      # native plan compiles cleanly for `head` and apply_pseudo_element
      # walks only the immediate children when collecting text/attrs.
      direct = false
      if head.end_with?(">")
        head = head[0..-2].rstrip
        direct = true
      end
      if pe.casecmp("::text").zero?
        [head, direct ? :direct_text : :text, nil]
      elsif (a = pe.match(/::attr\(([^)]+)\)/i))
        [head, direct ? :direct_attr : :attr, a[1].strip]
      else
        [head, :text_approx, nil]
      end
    end

    if AVAILABLE_DOM
      # Lightweight wrapper: two slots, the native doc + node id.
      # Walks like a Dom::Element so the rest of Scrapetor can treat
      # it the same.
      class Element
        attr_reader :doc, :id

        def initialize(doc, id, wrapper = nil)
          @doc     = doc
          @id      = id
          @wrapper = wrapper
          @dom_node = nil
        end

        # The DocumentWrapper governs the native arena and any lazy
        # Dom view used for mutations / fallback selectors. Surface it
        # so subclasses and nav helpers can stay coherent.
        def wrapper
          @wrapper ||= @doc.instance_variable_get(:@__scrapetor_wrapper)
        end

        def element?
          @dom_node ? @dom_node.element? : @doc.node_is_element(@id)
        end

        def text?;    @dom_node ? @dom_node.text?    : @doc.node_type(@id) == 3; end
        def comment?; @dom_node ? @dom_node.comment? : @doc.node_type(@id) == 8; end
        def document?; @dom_node ? @dom_node.document? : @doc.node_type(@id) == 9; end

        def name
          dom_node? ? @dom_node.name : @doc.node_name(@id)
        end
        alias node_name name
        alias tag_name name

        def [](key)
          dom_node? ? @dom_node[key.to_s] : @doc.node_attr(@id, key.to_s)
        end
        alias get_attribute []
        alias attribute_value []

        def attributes
          if dom_node?
            @dom_node.attributes
          else
            @doc.node_attributes(@id)
          end
        end

        # Lightweight pair returned from `attribute_nodes` / `attribute`.
        # The `.text` / `.content` / `.inner_text` accessors mirror what
        # Nokogiri's Nokogiri::XML::Attr exposes — production parser code
        # iterates `node.attribute_nodes` and reads `.text` on each.
        AttrNode = Struct.new(:name, :value) do
          def text;       value.to_s; end
          alias content    text
          alias inner_text text
          def to_s
            %Q{#{name}="#{value}"}
          end
        end

        def attribute_nodes
          if dom_node?
            @dom_node.attribute_nodes
          else
            attributes.map { |k, v| AttrNode.new(k, v) }
          end
        end

        def attribute(name)
          if dom_node?
            @dom_node.attribute(name)
          else
            v = self[name]
            v && AttrNode.new(name.to_s, v)
          end
        end

        def keys;    dom_node? ? @dom_node.keys : attributes.keys; end
        def values;  dom_node? ? @dom_node.values : attributes.values; end
        def has_attribute?(k)
          if dom_node?
            @dom_node.has_attribute?(k)
          else
            !@doc.node_attr(@id, k.to_s).nil?
          end
        end
        alias key? has_attribute?

        # Stable identity used to relocate this node inside a lazy Dom
        # view after the document switches to dom-mode. Builds the same
        # `/tag[idx]/.../tag[@id='x']` shape we already exposed publicly.
        # Memoized per-id on the document wrapper so a fallback-heavy
        # parser doesn't pay the O(depth*siblings) walk per at_css call.
        def path
          w = wrapper
          if w && (cached = w.cached_path(@id))
            return cached
          end
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
          str = "/" + parts.join("/")
          w.store_path(@id, str) if w
          str
        end

        def fragment?; false; end
        def cdata?;    false; end
        def processing_instruction?; false; end

        def text
          dom_node? ? @dom_node.text : @doc.node_text(@id)
        end
        alias content text
        alias inner_text text

        def parent
          if dom_node?
            p = @dom_node.parent
            return nil if p.nil?
            return nil unless p.respond_to?(:element?) && p.element?
            wrap_dom(p)
          else
            pid = @doc.node_parent(@id)
            pid ? Element.new(@doc, pid, wrapper) : nil
          end
        end

        def children
          if dom_node?
            @dom_node.children.map { |c| wrap_dom(c) }
          else
            @doc.node_children(@id).map { |cid| Element.new(@doc, cid, wrapper) }
          end
        end

        def element_children
          if dom_node?
            @dom_node.element_children.map { |c| wrap_dom(c) }
          else
            @doc.node_element_children(@id).map { |cid| Element.new(@doc, cid, wrapper) }
          end
        end
        alias elements element_children

        def first_element_child
          if dom_node?
            c = @dom_node.first_element_child
            c && wrap_dom(c)
          else
            ids = @doc.node_element_children(@id)
            ids.empty? ? nil : Element.new(@doc, ids.first, wrapper)
          end
        end

        def last_element_child
          if dom_node?
            c = @dom_node.last_element_child
            c && wrap_dom(c)
          else
            ids = @doc.node_element_children(@id)
            ids.empty? ? nil : Element.new(@doc, ids.last, wrapper)
          end
        end

        def next_sibling
          if dom_node?
            n = @dom_node.next_sibling
            n && wrap_dom(n)
          else
            nid = @doc.node_next_sibling(@id)
            nid ? Element.new(@doc, nid, wrapper) : nil
          end
        end

        def previous_sibling
          if dom_node?
            n = @dom_node.previous_sibling
            n && wrap_dom(n)
          else
            nid = @doc.node_prev_sibling(@id)
            nid ? Element.new(@doc, nid, wrapper) : nil
          end
        end

        def next_element_sibling
          if dom_node?
            n = @dom_node.next_element_sibling
            n && wrap_dom(n)
          else
            cur = @doc.node_next_sibling(@id)
            while cur && !@doc.node_is_element(cur)
              cur = @doc.node_next_sibling(cur)
            end
            cur ? Element.new(@doc, cur, wrapper) : nil
          end
        end

        def previous_element_sibling
          if dom_node?
            n = @dom_node.previous_element_sibling
            n && wrap_dom(n)
          else
            cur = @doc.node_prev_sibling(@id)
            while cur && !@doc.node_is_element(cur)
              cur = @doc.node_prev_sibling(cur)
            end
            cur ? Element.new(@doc, cur, wrapper) : nil
          end
        end

        def classes
          dom_node? ? @dom_node.classes : @doc.node_classes(@id)
        end

        def has_class?(klass); classes.include?(klass.to_s); end

        # ----- selectors -----

        def css(selector)
          str = selector.to_s
          # Super-fast path: single-group String with no pseudo-element
          # and no comma — the shape that dominates css() calls on
          # listing-style parsers. Skip peel + heterogeneous checks.
          if !@dom_node && !str.include?(",") && !str.include?("::")
            w = @wrapper
            if w
              plan = w.compiled_plan(str)
              if plan
                ids = @doc.run_chain(plan, @id)
                return ids.map { |nid| Element.new(@doc, nid, w) }
              end
            end
          end
          # If the selector is a comma-list whose groups disagree on
          # pseudo-element shape (one group ends in `::text` / `> ::text`
          # while another doesn't), peel and run each group separately
          # and concatenate the heterogeneous results.
          if str.include?(",") && str.include?("::") &&
             Native.heterogeneous_pseudo_groups?(str)
            return Native.split_selector_groups(str).flat_map { |g| css(g).to_a }
          end
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          # Fast path only handles the "concatenated subtree text/attr"
          # forms. :direct_text / :direct_attr need per-child walking,
          # which apply_pseudo_element handles after a normal match.
          if kind && %i[text text_approx attr].include?(kind) && !dom_node?
            w = wrapper
            plan = w ? w.compiled_plan(stripped) : Native.compile_selector_chain(stripped)
            if plan && !stripped.include?(",")
              ids = @doc.run_chain(plan, @id)
              return case kind
                     when :text, :text_approx
                       wire_text_parents!(@doc.bulk_text(ids), ids, w)
                     when :attr
                       wire_text_parents!(@doc.bulk_attr(ids, arg), ids, w)
                     end
            end
          end
          nodes = css_native_or_fallback(stripped)
          apply_pseudo_element(nodes, kind, arg)
        end

        def at_css(selector)
          str = selector.to_s
          # Super-fast path: single-group String with no pseudo-element
          # and no comma. Avoids the peel + heterogeneous checks + the
          # routing-method dispatch. Validated as the hot shape on
          # at_css-heavy parsers; routing overhead was ~37% of total
          # time before this short-circuit.
          if !@dom_node && !str.include?(",") && !str.include?("::")
            w = @wrapper
            if w
              plan = w.compiled_plan(str)
              if plan
                ids = @doc.run_chain(plan, @id)
                return nil if ids.empty?
                return Element.new(@doc, ids.first, w)
              end
            end
          end
          if str.include?(",") && str.include?("::") &&
             Native.heterogeneous_pseudo_groups?(str)
            Native.split_selector_groups(str).each do |g|
              hit = at_css(g)
              return hit if hit
            end
            return nil
          end
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          nodes = css_native_or_fallback(stripped, limit_one: true)
          return nil if nodes.empty?
          return nodes.first unless kind
          apply_pseudo_element(nodes, kind, arg).first
        end
        alias at at_css
        alias search css

        def xpath(_expr); []; end
        def at_xpath(_expr); nil; end

        def matches?(selector)
          # Walk up self's ancestor-or-self set; cheap version of
          # checking whether *this* node matches the selector.
          doc = wrapper ? wrapper : nil
          if doc
            doc.css(selector).any? { |n| n == self }
          else
            # No wrapper available — fall back to checking via parent.
            false
          end
        end

        # ----- serialization -----

        def inner_html
          if dom_node?
            @dom_node.inner_html
          else
            element_children.map(&:to_html).join + text_only_children
          end
        end

        def outer_html
          if dom_node?
            @dom_node.outer_html
          else
            attr_str = attributes.map { |k, v| %( #{k}="#{Dom.escape_attr(v)}") }.join
            if Dom::VOID.include?(name) && @doc.node_children(@id).empty?
              "<#{name}#{attr_str}>"
            else
              "<#{name}#{attr_str}>#{inner_html}</#{name}>"
            end
          end
        end
        alias to_html outer_html
        alias to_xml outer_html
        alias to_s outer_html

        def node_type
          dom_node? ? @dom_node.node_type : @doc.node_type(@id)
        end
        alias type node_type

        def ==(other)
          return true if equal?(other)
          return false unless other.is_a?(Element)
          if dom_node? && other.dom_backed?
            @dom_node.equal?(other.dom_node)
          elsif !dom_node? && !other.dom_backed?
            @doc.equal?(other.doc) && @id == other.id
          else
            false
          end
        end
        alias eql? ==

        def hash
          if dom_node?
            @dom_node.object_id
          else
            [@doc.object_id, @id].hash
          end
        end

        def fingerprint
          Scrapetor::Fingerprint.structural(self)
        end

        # ----- mutation API -----
        #
        # The native arena DOM is immutable by design (it gives us the
        # zero-copy parse + 137x Lexbor lead). Mutations promote the
        # document to a Ruby `Dom::Document` once, then operate on the
        # equivalent Dom node. Reads continue to work on either side.

        def []=(key, value)
          ensure_dom!
          @dom_node[key.to_s] = value.nil? ? nil : value.to_s
          value
        end
        alias set_attribute []=

        def remove_attribute(key)
          ensure_dom!
          @dom_node.remove_attribute(key.to_s)
          self
        end
        alias delete_attribute remove_attribute

        def add_class(klass)
          ensure_dom!
          @dom_node.add_class(klass.to_s)
          self
        end
        alias append_class add_class

        def remove_class(klass = nil)
          ensure_dom!
          @dom_node.remove_class(klass && klass.to_s)
          self
        end

        def content=(text)
          ensure_dom!
          @dom_node.content = text.to_s
          text
        end
        alias text= content=

        def inner_html=(html)
          ensure_dom!
          @dom_node.inner_html = html.to_s
          html
        end

        def add_child(node_or_html)
          ensure_dom!
          @dom_node.add_child(unwrap_for_mutation(node_or_html))
        end
        alias << add_child

        def add_previous_sibling(node_or_html)
          ensure_dom!
          @dom_node.add_previous_sibling(unwrap_for_mutation(node_or_html))
        end
        alias before add_previous_sibling

        def add_next_sibling(node_or_html)
          ensure_dom!
          @dom_node.add_next_sibling(unwrap_for_mutation(node_or_html))
        end
        alias after add_next_sibling

        def replace(node_or_html)
          ensure_dom!
          @dom_node.replace(unwrap_for_mutation(node_or_html))
        end
        alias swap replace
        alias replace_with replace

        # Detach this element from its parent. When we're still on the
        # native arena, mutate it in place — that avoids the cross-DOM
        # path lookup (which can't always pin down a node on HTML where
        # the native vs Ruby SAX parsers disagree about whitespace or
        # implicit close tags). Once the document has been promoted to
        # Ruby Dom by some other mutation, delegate to that side.
        def remove
          if @dom_node
            @dom_node.remove
          else
            @doc.node_remove(@id)
          end
          self
        end
        alias unlink remove
        alias delete remove

        # Wrap this element in a parsed HTML fragment whose deepest
        # descendant becomes the new parent. Matches Nokogiri's
        # Node#wrap semantics.
        def wrap(html_or_node)
          ensure_dom!
          @dom_node.wrap(html_or_node) if @dom_node.respond_to?(:wrap)
          self
        end

        def traverse(&block)
          if block_given?
            yield self
            element_children.each { |c| c.traverse(&block) }
            self
          else
            enum_for(:traverse)
          end
        end

        # Internal: was this Element already promoted to a Dom::Element?
        def dom_backed?
          dom_node?
        end

        def dom_node
          @dom_node
        end

        # Public version of the lazy dom-promotion step. NodeSet#remove
        # uses it to resolve every node to its Dom equivalent BEFORE the
        # first mutation, so subsequent removals don't shift the path
        # index under their feet.
        def promote_to_dom!
          ensure_dom!
          @dom_node
        end

        def apply_pseudo_element(nodes, kind, arg)
          case kind
          when nil then nodes
          when :text, :text_approx
            nodes.map do |n|
              t = Scrapetor::TextNode.new(n.respond_to?(:text) ? n.text.to_s : n.to_s)
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              t
            end
          when :direct_text
            out = []
            nodes.each do |n|
              str = direct_text_of(n)
              tn = Scrapetor::TextNode.new(str)
              tn.parent_node = n if n.respond_to?(:element?) && n.element?
              out << tn
            end
            out
          when :attr
            nodes.map do |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              next nil if v.nil?
              t = Scrapetor::TextNode.new(v)
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              t
            end
          when :direct_attr
            out = []
            nodes.each do |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              next if v.nil?
              tn = Scrapetor::TextNode.new(v)
              tn.parent_node = n if n.respond_to?(:element?) && n.element?
              out << tn
            end
            out
          end
        end

        # Direct text-node children only — handles the convention
        # `parent > ::text` (and `> ::attr(x)`) where descendant text
        # inside child elements must NOT be included.
        DOM_TYPE_TEXT = 3
        def direct_text_of(n)
          buf = +""
          if n.is_a?(Element) && !n.send(:dom_node?)
            doc = @doc
            cid = doc.node_first_child(n.id)
            while cid
              if doc.node_type(cid) == DOM_TYPE_TEXT
                buf << doc.node_text(cid).to_s
              end
              cid = doc.node_next_sibling(cid)
            end
          elsif n.respond_to?(:children)
            n.children.each do |c|
              if c.respond_to?(:text?) && c.text?
                buf << (c.respond_to?(:text) ? c.text.to_s : c.to_s)
              elsif !c.respond_to?(:element?) || !c.element?
                buf << c.to_s
              end
            end
          end
          buf
        end

        # Helper for Element#css: take a bulk_text / bulk_attr result
        # and wire each TextNode's parent to the matching Element wrapper.
        def wire_text_parents!(values, ids, w)
          i = 0
          n = values.length
          while i < n
            v = values[i]
            if v.is_a?(Scrapetor::TextNode)
              v.parent_node = Element.new(@doc, ids[i], w)
            end
            i += 1
          end
          values
        end

        private

        def dom_node?
          !@dom_node.nil?
        end

        # Promote this Element (and the underlying document) to the
        # Ruby DOM. After this, all reads and writes hit @dom_node and
        # the wrapper's @dom_doc rather than the native arena.
        #
        # Three-stage lookup, each weaker than the last but always
        # leaving the caller with a mutable Dom::Element to operate on:
        #   1. Strict path-based locate (well-formed HTML where both
        #      parsers produce the same element tree).
        #   2. DFS pre-order element-index lookup (handles parsers
        #      disagreeing on whitespace text nodes / implicit close
        #      tags — element-order is still stable).
        #   3. Isolated subtree parse — feed our own outer_html through
        #      the Ruby Dom parser and use the top-level element as the
        #      promoted node. Mutations propagate to subsequent reads
        #      via @dom_node (Element#outer_html reads from there), so
        #      the user's `node.inner_html = ...` etc. always work even
        #      if we can't pin the node back into the document's Dom.
        def ensure_dom!
          return @dom_node if @dom_node
          w = wrapper
          raise NotImplementedError, "Mutation requires a DocumentWrapper" if w.nil?
          w.switch_to_dom!
          @dom_node = w.locate_in_dom(path) ||
                      w.locate_dom_by_native_id(@id) ||
                      isolated_dom_clone
          raise NotImplementedError, "Cannot locate or clone equivalent node" if @dom_node.nil?
          @dom_node
        end

        def isolated_dom_clone
          html = to_html
          return nil if html.nil? || html.empty?
          frag = Scrapetor::Dom::Parser.fragment(html)
          frag.find { |n| n.respond_to?(:element?) && n.element? }
        end

        def wrap_dom(node)
          el = Element.new(@doc, @id, wrapper)
          el.instance_variable_set(:@dom_node, node)
          el
        end

        def unwrap_for_mutation(input)
          if input.is_a?(Element)
            input.dom_node || input.to_html
          elsif input.is_a?(Scrapetor::Node)
            inner = input.backing_node
            if inner.is_a?(Element)
              inner.dom_node || inner.to_html
            else
              inner
            end
          else
            input
          end
        end

        def text_only_children
          children = @doc.node_children(@id)
          children.filter_map do |cid|
            @doc.node_type(cid) == 3 ? @doc.node_text(cid) : nil
          end.join
        end

        # Try native first; fall back to the lazy Dom view on the
        # wrapper. Returns an Array of Element wrappers (native or
        # dom-backed).
        def css_native_or_fallback(selector_str, limit_one: false)
          if dom_node?
            # Text / comment / doctype dom nodes don't support .css —
            # NodeSet#children aggregates these alongside element nodes
            # and Nokogiri-shape code paths still pump them through the
            # subsequent `.css` call. Return an empty Array instead of
            # blowing up with "undefined method `css`".
            return [] unless @dom_node.respond_to?(:css)
            return @dom_node.css(selector_str).map { |n| wrap_dom(n) }
          end

          w = wrapper

          # Fast path: single-group selector with cached plan.
          if !selector_str.include?(",")
            plan = w ? w.compiled_plan(selector_str) : Native.compile_selector_chain(selector_str)
            if plan
              ids = @doc.run_chain(plan, @id)
              ids = ids.first(1) if limit_one
              return ids.map { |nid| Element.new(@doc, nid, w) }
            end
            # Single-group but failed to compile — try distributing
            # `:is(...)` alternatives into separate groups before bailing.
            expanded = Native.expand_is_groups(selector_str)
            if expanded.size > 1
              all = []
              seen = nil
              all_ok = true
              expanded.each do |g|
                plan = w ? w.compiled_plan(g) : Native.compile_selector_chain(g)
                if plan.nil?
                  all_ok = false
                  break
                end
                @doc.run_chain(plan, @id).each do |nid|
                  seen ||= {}
                  next if seen[nid]
                  seen[nid] = true
                  all << Element.new(@doc, nid, w)
                  break if limit_one
                end
                break if limit_one && !all.empty?
              end
              return all if all_ok
            end
            if w
              dom_scope = w.locate_in_dom(path) || w.fallback_dom
              list = dom_scope.css(selector_str).to_a
              list = list.first(1) if limit_one
              return list.map { |n| wrap_dom(n) }
            end
            return []
          end

          all = []
          seen = nil
          ok = true
          groups = Native.split_selector_groups(selector_str)
            .flat_map { |g| Native.expand_is_groups(g) }
          groups.each do |g|
            plan = w ? w.compiled_plan(g) : Native.compile_selector_chain(g)
            if plan.nil?
              ok = false
              break
            end
            @doc.run_chain(plan, @id).each do |nid|
              seen ||= {}
              next if seen[nid]
              seen[nid] = true
              all << Element.new(@doc, nid, w)
              break if limit_one
            end
            break if limit_one && !all.empty?
          end
          return all if ok

          if w
            dom_scope = w.locate_in_dom(path) || w.fallback_dom
            return dom_scope.css(selector_str).map { |n| wrap_dom(n) }
          end
          []
        end
      end

      # Document wrapper — wraps Native::Document and provides Dom-like
      # methods so `Scrapetor::Document#backing` can return one of these
      # interchangeably with `Dom::Document`.
      class DocumentWrapper
        attr_reader :native

        # The compile cache lives on the wrapper so repeated queries
        # (the common case in scraping pipelines, where the same set of
        # selectors run against thousands of pages) skip the parse +
        # native-plan build entirely. Sized to cover typical templates;
        # untouched entries fall off the back when we exceed cap.
        COMPILE_CACHE_CAP = 1024

        def initialize(native)
          @native = native
          # Back-pointer so Elements created from this wrapper can
          # find their way back without us threading `wrapper:` through
          # every navigation method.
          native.instance_variable_set(:@__scrapetor_wrapper, self) if native.respond_to?(:instance_variable_set)
          @dom_doc  = nil
          @dom_mode = false
          @compile_cache = {}
          # Path cache keyed by native node id. Stable until the tree
          # mutates (dom-mode flip clears it).
          @path_cache = {}
        end

        def cached_path(id)
          @path_cache[id]
        end

        def store_path(id, str)
          @path_cache[id] = str
        end

        # Look up (or compile) the native plan for a single selector group.
        # `nil` means "this group uses a feature the native engine
        # doesn't accept" — callers route those to the Ruby fallback.
        def compiled_plan(group_str)
          if (entry = @compile_cache[group_str])
            return entry == false ? nil : entry
          end
          plan = Native.compile_selector_chain(group_str)
          @compile_cache.shift if @compile_cache.size >= COMPILE_CACHE_CAP
          @compile_cache[group_str] = plan.nil? ? false : plan
          if plan.nil? && ENV["SCRAP_TRACE_FALLBACK"]
            warn "[scrap-fallback] #{group_str}"
          end
          plan
        end

        def element?; false; end
        def document?; true; end
        def name; "#document"; end

        def root
          rid = @native.root_id
          Element.new(@native, rid, self)
        end

        def root_element; root; end

        def text;     fallback_dom.text; end
        def content;  text; end

        # ----- selector entry points -----

        # `lazy_css` is the fast path that Document#css uses: it returns
        # raw ids when the native engine can handle the whole selector,
        # so the Element-wrap happens once-per-iteration instead of
        # once-per-result. Falls back to the eager `css` when native
        # can't handle the selector (kind, fallback dom, etc.).
        #
        # Returns a `LazyIds` struct OR an Array of strings (for
        # ::text/::attr) OR an Array of Element wrappers (when the
        # selector needs the Dom fallback).
        LazyIds = Struct.new(:wrapper, :native, :ids)

        def lazy_css(selector)
          str = selector.to_s
          # Heterogeneous pseudo groups: peel each group separately and
          # concatenate. Returns a flat Array of mixed Element/TextNode
          # results — callers wrap it in NodeSet via .to_a.
          if str.include?(",") && str.include?("::") &&
             Native.heterogeneous_pseudo_groups?(str)
            return Native.split_selector_groups(str).flat_map do |g|
              r = lazy_css(g)
              r.is_a?(LazyIds) ? r.ids.map { |nid| Element.new(@native, nid, self) } : r.to_a
            end
          end
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          if kind && %i[text text_approx attr].include?(kind) && !@dom_mode
            ids = native_ids(stripped)
            if ids
              return case kind
                     when :text, :text_approx
                       wire_parent_nodes!(@native.bulk_text(ids), ids)
                     when :attr
                       wire_parent_nodes!(@native.bulk_attr(ids, arg), ids)
                     end
            end
          end
          if !@dom_mode && kind.nil?
            ids = native_ids(stripped)
            return LazyIds.new(self, @native, ids) if ids
          end
          nodes = css_native_or_fallback(stripped)
          apply_transform(nodes, kind, arg)
        end

        # Set each TextNode's parent_node to the matching element it
        # came from. Production parser code (Google Light's organic
        # results, Yahoo's knowledge graph) chains `result.parent.css(...)`
        # to walk into siblings of a `::text` match — without a parent
        # ref the `.parent` returns nil and the next call crashes.
        def wire_parent_nodes!(values, ids)
          i = 0
          n = values.length
          while i < n
            v = values[i]
            if v.is_a?(Scrapetor::TextNode)
              v.parent_node = Element.new(@native, ids[i], self)
            end
            i += 1
          end
          values
        end

        def css(selector)
          str = selector.to_s
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          if kind && !@dom_mode
            ids = native_ids(stripped)
            if ids
              return case kind
                     when :text, :text_approx
                       wire_parent_nodes!(@native.bulk_text(ids), ids)
                     when :attr
                       wire_parent_nodes!(@native.bulk_attr(ids, arg), ids)
                     end
            end
          end
          nodes = css_native_or_fallback(stripped)
          apply_transform(nodes, kind, arg)
        end

        def at_css(selector)
          str = selector.to_s
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          nodes = css_native_or_fallback(stripped, limit_one: true)
          return nil if nodes.empty?
          return nodes.first unless kind
          apply_transform(nodes, kind, arg).first
        end
        alias at at_css

        # Run N selectors in ONE C call, returning an Array of results
        # parallel to `selectors`. Each result is either a `LazyIds`
        # (wrapped by Document#css as a lazy NodeSet) or an Array of
        # strings (for `::text` / `::attr` pseudo-elements). Selectors
        # the native engine can't compile fall through to the per-query
        # Ruby path; the rest amortise to one Ruby dispatch.
        def batch_css(doc, selectors)
          plans   = Array.new(selectors.size)
          kinds   = Array.new(selectors.size)
          args    = Array.new(selectors.size)
          natives = []
          native_to_orig = []
          fallback_indices = []

          selectors.each_with_index do |sel, i|
            str = sel.to_s
            stripped, kind, arg = Native.peel_pseudo_element(str)
            stripped = "*" if stripped.empty?
            kinds[i] = kind
            args[i]  = arg
            if @dom_mode || stripped.include?(",")
              fallback_indices << i
              next
            end
            plan = compiled_plan(stripped)
            if plan
              plans[i] = plan
              natives << plan
              native_to_orig << i
            else
              fallback_indices << i
            end
          end

          out = Array.new(selectors.size)

          # One C call across all native plans.
          unless natives.empty?
            id_lists = @native.batch_chain(natives, nil)
            id_lists.each_with_index do |ids, j|
              orig = native_to_orig[j]
              out[orig] = case kinds[orig]
                          when :text, :text_approx
                            wire_parent_nodes!(@native.bulk_text(ids), ids)
                          when :attr
                            wire_parent_nodes!(@native.bulk_attr(ids, args[orig]), ids)
                          else
                            LazyIds.new(self, @native, ids)
                          end
            end
          end

          # Per-selector Ruby path for the few that need it.
          fallback_indices.each do |i|
            out[i] = lazy_css(selectors[i])
          end

          # Wrap each result as Document#css would. Lazy NodeSet for
          # node-based results; pass strings through.
          out.map! do |r|
            if r.is_a?(LazyIds)
              Scrapetor::NodeSet.new(doc, r)
            else
              r
            end
          end
          out
        end

        def xpath(_expr); []; end
        def at_xpath(_expr); nil; end

        def traverse(&block)
          return enum_for(:traverse) unless block_given?
          root.traverse(&block)
          self
        end

        def to_html
          @dom_mode ? @dom_doc.to_html : @native.html
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

        # ----- internals for the mutation fallback -----

        def dom_mode?; @dom_mode; end

        # Build (once) and return the Ruby DOM view of this document.
        # Used by Element#css fallback when the selector exceeds the
        # native engine's grammar, and by Element mutations.
        def fallback_dom
          @dom_doc ||= Scrapetor::Dom::Parser.parse(@native.html)
        end

        # Promote the document to dom-mode. After this, css() runs only
        # against the Dom view (it is the source of truth for mutations
        # the user has already made).
        def switch_to_dom!
          fallback_dom
          @dom_mode = true
          # Cached paths may not survive a mutation series; let them
          # rebuild lazily after the switch.
          @path_cache = {}
        end

        # Walk a `/tag[idx]/.../tag[@id='x']` path inside the lazy Dom
        # view. Used by Element#ensure_dom! to relocate itself after
        # promotion.
        def locate_in_dom(path_str)
          doc = fallback_dom
          parts = path_str.to_s.split("/").reject(&:empty?)
          cur = doc
          parts.each do |part|
            if (m = part.match(/\A([\w-]+)\[@id='([^']+)'\]\z/))
              tag = m[1]; id = m[2]
              found = nil
              walk_elements(doc) do |el|
                if el.name == tag && el["id"] == id
                  found = el
                  break
                end
              end
              return nil if found.nil?
              cur = found
            elsif (m = part.match(/\A([\w-]+)\[(\d+)\]\z/))
              tag = m[1]; idx = m[2].to_i
              children = cur.respond_to?(:children) ? cur.children : []
              same = children.select { |c| c.respond_to?(:element?) && c.element? && c.name == tag }
              return nil if same.empty? || idx < 1 || idx > same.length
              cur = same[idx - 1]
            else
              return nil
            end
          end
          cur
        end

        # Robust cross-DOM lookup. Native ids enumerate every node in
        # the arena (text, comments, elements). Both parsers visit
        # ELEMENT nodes in document order, so the N-th element on the
        # native side is the N-th element on the Ruby side — even when
        # the two parsers disagree on whitespace text nodes or implicit
        # close-tag handling. Used as a fallback when the path-based
        # locator can't find a match.
        def locate_dom_by_native_id(native_id)
          @native_element_offset_map ||= build_native_element_offset_map
          offset = @native_element_offset_map[native_id]
          return nil if offset.nil?
          @dom_element_index ||= build_dom_element_index
          @dom_element_index[offset]
        end

        private

        def build_native_element_offset_map
          map = {}
          count = 0
          size = @native.size
          i = 0
          while i < size
            if @native.node_is_element(i)
              map[i] = count
              count += 1
            end
            i += 1
          end
          map
        end

        def build_dom_element_index
          list = []
          walk_elements(fallback_dom) { |el| list << el }
          list
        end

        public

        # Run the cached plan(s) for a selector and return the raw id
        # Array, or nil if any group needs the Ruby fallback. Used by
        # css() to feed bulk_text / bulk_attr without intermediate
        # Element allocations.
        def native_ids(selector_str)
          if !selector_str.include?(",")
            plan = compiled_plan(selector_str)
            return @native.run_chain(plan, nil) if plan
            expanded = Native.expand_is_groups(selector_str)
            return nil if expanded.size <= 1
            ids = []
            seen = nil
            expanded.each do |g|
              p = compiled_plan(g)
              return nil unless p
              @native.run_chain(p, nil).each do |nid|
                seen ||= {}
                next if seen[nid]
                seen[nid] = true
                ids << nid
              end
            end
            return ids
          end
          ids = []
          seen = nil
          groups = Native.split_selector_groups(selector_str)
            .flat_map { |g| Native.expand_is_groups(g) }
          groups.each do |g|
            plan = compiled_plan(g)
            return nil unless plan
            @native.run_chain(plan, nil).each do |nid|
              seen ||= {}
              next if seen[nid]
              seen[nid] = true
              ids << nid
            end
          end
          ids
        end

        def apply_transform(nodes, kind, arg)
          case kind
          when nil then nodes
          when :text, :text_approx
            nodes.map do |n|
              t = Scrapetor::TextNode.new(n.respond_to?(:text) ? n.text.to_s : n.to_s)
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              t
            end
          when :attr
            nodes.map do |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              next nil if v.nil?
              t = Scrapetor::TextNode.new(v)
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              t
            end
          when :direct_text
            nodes.map do |n|
              t = Scrapetor::TextNode.new(direct_text_of_any(n))
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              t
            end
          when :direct_attr
            out = []
            nodes.each do |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              next if v.nil?
              t = Scrapetor::TextNode.new(v)
              t.parent_node = n if n.respond_to?(:element?) && n.element?
              out << t
            end
            out
          end
        end

        # Direct text-node children of an element. Used at the
        # Document/wrapper level — accepts either a native Element or a
        # Dom-fallback node and pulls only the immediate text children.
        def direct_text_of_any(n)
          buf = +""
          if n.is_a?(Element) && !n.send(:dom_node?)
            cid = @native.node_first_child(n.id)
            while cid
              if @native.node_type(cid) == 3
                buf << @native.node_text(cid).to_s
              end
              cid = @native.node_next_sibling(cid)
            end
          elsif n.respond_to?(:children)
            n.children.each do |c|
              if c.respond_to?(:text?) && c.text?
                buf << (c.respond_to?(:text) ? c.text.to_s : c.to_s)
              elsif !c.respond_to?(:element?) || !c.element?
                buf << c.to_s
              end
            end
          end
          buf
        end

        private

        def walk_elements(scope, &block)
          children = scope.respond_to?(:children) ? scope.children : []
          children.each do |c|
            next unless c.respond_to?(:element?) && c.element?
            yield c
            walk_elements(c, &block)
          end
        end

        def css_native_or_fallback(selector_str, limit_one: false)
          # Once in dom-mode, native arena is stale wrt user mutations.
          if @dom_mode
            doc = fallback_dom
            list = doc.css(selector_str).to_a
            list = list.first(1) if limit_one
            return list.map { |n| wrap_dom_node(n) }
          end

          # Fast path: single-group selector with cached plan.
          if !selector_str.include?(",")
            plan = compiled_plan(selector_str)
            if plan
              ids = @native.run_chain(plan, nil)
              ids = ids.first(1) if limit_one
              return ids.map { |nid| Element.new(@native, nid, self) }
            end
            expanded = Native.expand_is_groups(selector_str)
            if expanded.size > 1
              all = []
              seen = nil
              all_ok = true
              expanded.each do |g|
                p = compiled_plan(g)
                if p.nil?
                  all_ok = false
                  break
                end
                @native.run_chain(p, nil).each do |nid|
                  seen ||= {}
                  next if seen[nid]
                  seen[nid] = true
                  all << Element.new(@native, nid, self)
                  break if limit_one
                end
                break if limit_one && !all.empty?
              end
              return all if all_ok
            end
            # Not natively supported — route to Dom fallback.
            list = fallback_dom.css(selector_str).to_a
            list = list.first(1) if limit_one
            return list.map { |n| wrap_dom_node(n) }
          end

          # Comma-separated groups.
          all = []
          seen = nil
          ok = true
          groups = Native.split_selector_groups(selector_str)
            .flat_map { |g| Native.expand_is_groups(g) }
          groups.each do |g|
            plan = compiled_plan(g)
            if plan.nil?
              ok = false
              break
            end
            @native.run_chain(plan, nil).each do |nid|
              seen ||= {}
              next if seen[nid]
              seen[nid] = true
              all << Element.new(@native, nid, self)
              break if limit_one
            end
            break if limit_one && !all.empty?
          end
          return all if ok

          list = fallback_dom.css(selector_str).to_a
          list = list.first(1) if limit_one
          list.map { |n| wrap_dom_node(n) }
        end

        def wrap_dom_node(dom_el)
          el = Element.new(@native, 0, self)
          el.instance_variable_set(:@dom_node, dom_el)
          el
        end
      end
    end  # if AVAILABLE_DOM

    # ----- selector compilation: CSS string -> chain of native plans -----

    # Each plan entry is `[selector_atom, combinator_or_nil]`.
    # selector_atom = [tag, classes, id, attrs]
    # selector_atom = [tag, classes, id, attrs, pseudo_data]   # extended
    # pseudo_data   = nil | [flags, nth_a, nth_b, nth_type_a, nth_type_b,
    #                        not_inner, is_inner, has_inner]
    # combinator    = nil | "descendant" | "child"
    #
    # Returns nil (never raises) when the selector contains a feature
    # the native engine doesn't accept (sibling combinator, comma at
    # top level, pseudo with a non-simple inner selector). Callers route
    # those to the Ruby DOM fallback in this same gem.

    # Mirrors C_PS_* in ext/scrapetor/native/scrapetor_dom.c. Keep in sync.
    NATIVE_PSEUDO_FLAGS = {
      "first-child"       => 1 << 0,
      "last-child"        => 1 << 1,
      "only-child"        => 1 << 2,
      "first-of-type"     => 1 << 3,
      "last-of-type"      => 1 << 4,
      "only-of-type"      => 1 << 5,
      "empty"             => 1 << 6,
      "root"              => 1 << 7,
      "checked"           => 1 << 8,
      "disabled"          => 1 << 9,
      "enabled"           => 1 << 10,
      "required"          => 1 << 11,
      "optional"          => 1 << 12,
      "read-only"         => 1 << 13,
      "read-write"        => 1 << 14,
      "any-link"          => 1 << 15,
      "link"              => 1 << 15,
      "scope"             => 1 << 23
    }.freeze

    NATIVE_NTH_BITS = {
      "nth-child"          => 1 << 16,
      "nth-last-child"     => 1 << 17,
      "nth-of-type"        => 1 << 18,
      "nth-last-of-type"   => 1 << 19
    }.freeze

    NATIVE_PSEUDO_FALLBACK = :__scrapetor_native_fallback__

    def self.compile_selector_chain(selector_str)
      plan = Scrapetor::Selector.compile(selector_str)
      out = []
      plan.each do |atom|
        pseudo_data = nil
        if atom.pseudos && !atom.pseudos.empty?
          pseudo_data = native_pseudo_data(atom.pseudos)
          return nil if pseudo_data == NATIVE_PSEUDO_FALLBACK
        end
        sel = [
          atom.tag ? atom.tag.to_s : nil,
          atom.classes,
          atom.id,
          atom.attrs,
          pseudo_data
        ]
        combo =
          case atom.combinator
          when :descendant then "descendant"
          when :child      then "child"
          when :adj        then "adjacent"
          when :gen        then "sibling"
          else nil
          end
        out << [sel, combo]
      end
      out
    rescue ArgumentError
      nil
    end

    # Compile the Atom#pseudos list into the eight-element Array the C
    # side reads. Returns NATIVE_PSEUDO_FALLBACK if any pseudo is
    # outside the native subset (in which case the whole chain falls
    # back to Ruby).
    def self.native_pseudo_data(pseudos)
      flags = 0
      nth_a = nth_b = 0
      nth_type_a = nth_type_b = 0
      not_inner = []
      is_inner = []
      has_inner = []
      not_has_inner = []
      has_child_inner = []
      not_has_child_inner = []
      has_chain_inner = []

      pseudos.each do |name, arg, double_colon|
        return NATIVE_PSEUDO_FALLBACK if double_colon

        if (bit = NATIVE_PSEUDO_FLAGS[name])
          flags |= bit
        elsif (bit = NATIVE_NTH_BITS[name])
          a, b = Scrapetor::Selector.parse_nth(arg)
          return NATIVE_PSEUDO_FALLBACK unless a
          flags |= bit
          if name == "nth-of-type" || name == "nth-last-of-type"
            nth_type_a, nth_type_b = a, b
          else
            nth_a, nth_b = a, b
          end
        elsif name == "not"
          # `:not(:has(X, Y))` — common scrape pattern. Rather than
          # forcing a Ruby Dom fallback (which is ~3-5 ms per call on a
          # 100KB page), recognise the shape at compile time and emit
          # a C_PS_NOT_HAS bit on the outer atom. The C side checks
          # "no descendant matches any of these simple atoms" — same
          # cost as C_PS_HAS, just inverted.
          if (nh = parse_not_has_form(arg))
            not_has_inner.concat(nh)
            flags |= (1 << 24)
            next
          end
          # `:not(:has(> X))` direct-child variant.
          if (nhc = parse_not_has_child_form(arg))
            not_has_child_inner.concat(nhc)
            flags |= (1 << 26)
            next
          end
          inner = native_inner_simples(arg)
          return NATIVE_PSEUDO_FALLBACK if inner == NATIVE_PSEUDO_FALLBACK
          not_inner.concat(inner)
          flags |= (1 << 20)
        elsif name == "is" || name == "matches" || name == "where"
          inner = native_inner_simples(arg)
          return NATIVE_PSEUDO_FALLBACK if inner == NATIVE_PSEUDO_FALLBACK
          is_inner.concat(inner)
          flags |= (1 << 21)
        elsif name == "has"
          # `:has(>::text)` / `:has(::text)` — "node has a direct
          # text-node child". Non-standard but appears in production
          # parsers. Maps to a one-bit flag the C side evaluates with
          # a single child walk.
          if has_text_child_form?(arg)
            flags |= (1 << 28)
            next
          end
          # `:has(> X, > Y)` — leading combinator inside :has. The
          # arg's compile output starts with `:scope` (compile()
          # desugars the leading `>`), giving each group two atoms.
          # native_inner_simples requires a single atom, so detect
          # this shape explicitly and lift the *child* atoms into
          # has_child_inner.
          if (hc = parse_has_child_form(arg))
            has_child_inner.concat(hc)
            flags |= (1 << 25)
            next
          end
          # `:is(...)` inside :has: distribute alternatives so an inner
          # like `:is(h2, span).a-color-base` becomes
          # `h2.a-color-base, span.a-color-base` before we hand it to
          # native_inner_simples (which needs single-atom groups). Force
          # distribution even for single-atom alternatives — the comma-
          # joined form is exactly the shape native_inner_simples wants.
          arg_expanded = Native.split_selector_groups(arg)
            .flat_map { |g| Native.expand_is_groups(g, force: true) }
            .join(", ")
          inner = native_inner_simples(arg_expanded)
          if inner != NATIVE_PSEUDO_FALLBACK
            has_inner.concat(inner)
            flags |= (1 << 22)
            next
          end
          # `:has(X Y)` — single chain with descendant/child combinators
          # between simple atoms. Lift it into has_chain_inner so the
          # C engine runs the descendant + chain-verify path natively.
          if (chain = parse_has_chain_form(arg))
            has_chain_inner = chain
            flags |= (1 << 27)
            next
          end
          return NATIVE_PSEUDO_FALLBACK
        else
          return NATIVE_PSEUDO_FALLBACK
        end
      end

      [flags, nth_a, nth_b, nth_type_a, nth_type_b, not_inner, is_inner, has_inner,
       not_has_inner, has_child_inner, not_has_child_inner, has_chain_inner]
    end

    # `:has(>::text)` / `:has(::text)` — "node has at least one direct
    # text-node child". The compile would otherwise reject the bare
    # pseudo-element inside :has, forcing the whole selector to the
    # Ruby Dom fallback. Cheap-as-shrimp shape detector — just trims
    # whitespace and an optional leading `>`.
    def self.has_text_child_form?(arg)
      return false if arg.nil?
      s = arg.strip
      s = s[1..].lstrip if s.start_with?(">")
      s == "::text"
    end

    # `:has(X Y)` — single chain (no commas, no leading combinator). The
    # arg's compile output is multiple atoms joined by descendant/child
    # combinators. Returns an Array of [simple_atom_entry, combo_str]
    # pairs (combo_str is "descendant" / "child" / nil). Rejects forms
    # native_inner_simples already handles (single atom) and forms that
    # need recursive pseudos.
    def self.parse_has_chain_form(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      return nil if groups.size != 1
      plan = Scrapetor::Selector.compile(groups.first)
      return nil if plan.size < 2
      out = []
      plan.each_with_index do |atom, idx|
        leaf_pseudo = nil
        if atom.pseudos && !atom.pseudos.empty?
          leaf_pseudo = native_leaf_pseudo_data(atom.pseudos)
          return nil if leaf_pseudo.nil?
        end
        entry = [atom.tag ? atom.tag.to_s : nil, atom.classes, atom.id, atom.attrs]
        entry << leaf_pseudo if leaf_pseudo
        combo =
          case atom.combinator
          when :descendant then "descendant"
          when :child      then "child"
          when nil         then (idx.zero? ? nil : "descendant")
          else                  nil
          end
        # Adjacent / general sibling combinators inside :has are
        # currently not supported by the chain matcher — bail.
        return nil if atom.combinator && %i[adj gen].include?(atom.combinator)
        out << [entry, combo]
      end
      out
    rescue ArgumentError
      nil
    end

    # `:has(> X, > Y)` — every group of the argument must be of shape
    # `:scope > simple`. Returns the simple atoms (each is the right
    # side of the `>`) if so, nil otherwise.
    def self.parse_has_child_form(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      out = []
      groups.each do |g|
        gs = g.strip
        return nil unless gs.start_with?(">")
        inner = gs[1..].lstrip
        plan = Scrapetor::Selector.compile(inner)
        return nil if plan.size != 1
        atom = plan.first
        leaf_pseudo = nil
        if atom.pseudos && !atom.pseudos.empty?
          leaf_pseudo = native_leaf_pseudo_data(atom.pseudos)
          return nil if leaf_pseudo.nil?
        end
        entry = [atom.tag ? atom.tag.to_s : nil, atom.classes, atom.id, atom.attrs]
        entry << leaf_pseudo if leaf_pseudo
        out << entry
      end
      out
    rescue ArgumentError
      nil
    end

    # `:not(:has(> X))` — direct-child negative form.
    def self.parse_not_has_child_form(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      return nil if groups.size != 1
      plan = Scrapetor::Selector.compile(groups.first)
      return nil if plan.size != 1
      atom = plan.first
      return nil unless atom.pseudos && atom.pseudos.size == 1
      name, inner_arg, double_colon = atom.pseudos.first
      return nil if double_colon || name != "has"
      return nil if atom.tag || !atom.classes.empty? || atom.id || !atom.attrs.empty?
      parse_has_child_form(inner_arg)
    rescue ArgumentError
      nil
    end

    # Inspect a `:not(...)` argument; if the argument compiles to exactly
    # `:has(simple, simple, ...)` (no other tag/class/id/attr constraints
    # outside the :has), return the array of inner simple-atom forms so
    # the caller can lift them into the C_PS_NOT_HAS path. Returns nil
    # for anything else.
    def self.parse_not_has_form(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      return nil if groups.size != 1
      plan = Scrapetor::Selector.compile(groups.first)
      return nil if plan.size != 1
      atom = plan.first
      return nil unless atom.pseudos && atom.pseudos.size == 1
      name, inner_arg, double_colon = atom.pseudos.first
      return nil if double_colon || name != "has"
      return nil if atom.tag || !atom.classes.empty? || atom.id || !atom.attrs.empty?
      inner = native_inner_simples(inner_arg)
      return nil if inner == NATIVE_PSEUDO_FALLBACK
      inner
    rescue ArgumentError
      nil
    end

    # Compile an inner-selector argument (`:not(.x, :empty, .y[z])`) into
    # an array of simple-atom descriptors the C engine can read. Each
    # inner is `[tag, classes, id, attrs]` or, when pseudo flags are
    # present, `[tag, classes, id, attrs, leaf_pseudo_data]`. Combinators
    # and recursive pseudos (a `:not` inside a `:not`) still force the
    # Ruby fallback — the C side only flattens one level deep.
    def self.native_inner_simples(arg, depth = 0)
      return NATIVE_PSEUDO_FALLBACK if arg.nil? || arg.empty?
      return NATIVE_PSEUDO_FALLBACK if depth > 4
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      out = []
      groups.each do |g|
        plan = Scrapetor::Selector.compile(g)
        return NATIVE_PSEUDO_FALLBACK if plan.size != 1
        atom = plan.first
        # `:has(:is(X, Y))` / `:not(:is(X, Y))` etc.: unwrap a pure
        # `:is(...)` atom into its alternatives so the inner pool
        # receives the leaf simples without the recursive :is.
        if pure_is_atom?(atom)
          inner_arg = atom.pseudos.first[1]
          sub = native_inner_simples(inner_arg, depth + 1)
          return NATIVE_PSEUDO_FALLBACK if sub == NATIVE_PSEUDO_FALLBACK
          out.concat(sub)
          next
        end
        leaf_pseudo = nil
        if atom.pseudos && !atom.pseudos.empty?
          # Try the nested (one-level-recursive) shape first — accepts
          # `:not(simple)` / `:has(simple)` / `:not(:has(simple))` on the
          # inner atom, lifting them into inner pools on the inner
          # c_simple_atom. Falls back to leaf-only if that doesn't apply.
          leaf_pseudo = native_inner_simple_pseudo(atom.pseudos) ||
                        native_leaf_pseudo_data(atom.pseudos)
          return NATIVE_PSEUDO_FALLBACK if leaf_pseudo.nil?
        end
        entry = [atom.tag ? atom.tag.to_s : nil, atom.classes, atom.id, atom.attrs]
        entry << leaf_pseudo if leaf_pseudo
        out << entry
      end
      out
    rescue ArgumentError
      NATIVE_PSEUDO_FALLBACK
    end

    # An atom that is *only* `:is(...)` — no tag/class/id/attrs and no
    # other pseudos — so the `:is` wraps a list of alternatives that
    # can be unwrapped into the surrounding inner pool. Anything else
    # on the atom (e.g. `.x:is(...)`) would change semantics and isn't
    # eligible for this rewrite.
    def self.pure_is_atom?(atom)
      return false if atom.tag || !atom.classes.empty? || atom.id || !atom.attrs.empty?
      return false unless atom.pseudos && atom.pseudos.size == 1
      name, _arg, double_colon = atom.pseudos.first
      !double_colon && %w[is matches where].include?(name)
    end

    # Build the extended pseudo_data slot for a c_simple_atom that
    # itself carries `:not(simple)` / `:has(simple)` / `:not(:has(simple))`
    # constraints. The C layer reads optional indices 5, 6, 7 as
    # inner_not / inner_has / inner_not_has pools and applies them in
    # matches_simple_atom. Returns nil when the shape isn't supported
    # (sibling combinators inside, recursive pseudos beyond one level,
    # etc.) — the caller falls back to native_leaf_pseudo_data which
    # rejects the atom entirely if leaves aren't enough.
    def self.native_inner_simple_pseudo(pseudos)
      flags = 0
      nth_a = nth_b = 0
      nth_type_a = nth_type_b = 0
      inner_not = []
      inner_has = []
      inner_not_has = []
      pseudos.each do |name, arg, double_colon|
        return nil if double_colon
        if (bit = NATIVE_PSEUDO_FLAGS[name])
          flags |= bit
        elsif (bit = NATIVE_NTH_BITS[name])
          a, b = Scrapetor::Selector.parse_nth(arg)
          return nil unless a
          flags |= bit
          if name == "nth-of-type" || name == "nth-last-of-type"
            nth_type_a, nth_type_b = a, b
          else
            nth_a, nth_b = a, b
          end
        elsif name == "not"
          # `:not(:has(simple))` → inner_not_has
          if (nh = parse_inner_not_has_form(arg))
            inner_not_has.concat(nh)
            next
          end
          sub = inner_pool_for(arg)
          return nil if sub.nil?
          inner_not.concat(sub)
        elsif name == "has"
          sub = inner_pool_for(arg)
          return nil if sub.nil?
          inner_has.concat(sub)
        else
          return nil
        end
      end
      out = [flags, nth_a, nth_b, nth_type_a, nth_type_b]
      # Pad with empty arrays as needed so the C layer indexes work.
      append_inner = lambda do |target, arr|
        out << arr if !arr.empty? || target < out.length
      end
      out << inner_not if !inner_not.empty? || !inner_has.empty? || !inner_not_has.empty?
      out << inner_has if !inner_has.empty? || !inner_not_has.empty?
      out << inner_not_has if !inner_not_has.empty?
      out
    end

    # Compile a `:not(arg)` / `:has(arg)` payload as a list of leaf
    # simple atoms (no further pseudo recursion). Used to fill an inner
    # pool on a c_simple_atom — limit one level deep.
    def self.inner_pool_for(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      out = []
      groups.each do |g|
        plan = Scrapetor::Selector.compile(g)
        return nil if plan.size != 1
        atom = plan.first
        if pure_is_atom?(atom)
          sub = inner_pool_for(atom.pseudos.first[1])
          return nil if sub.nil?
          out.concat(sub)
          next
        end
        leaf_pseudo = nil
        if atom.pseudos && !atom.pseudos.empty?
          leaf_pseudo = native_leaf_pseudo_data(atom.pseudos)
          return nil if leaf_pseudo.nil?
        end
        entry = [atom.tag ? atom.tag.to_s : nil, atom.classes, atom.id, atom.attrs]
        entry << leaf_pseudo if leaf_pseudo
        out << entry
      end
      out
    rescue ArgumentError
      nil
    end

    # `:not(:has(simple))` payload — used by inner_simple_pseudo to lift
    # the nested negation into inner_not_has on the simple atom.
    def self.parse_inner_not_has_form(arg)
      return nil if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      return nil if groups.size != 1
      plan = Scrapetor::Selector.compile(groups.first)
      return nil if plan.size != 1
      atom = plan.first
      return nil unless atom.pseudos && atom.pseudos.size == 1
      name, inner_arg, double_colon = atom.pseudos.first
      return nil if double_colon || name != "has"
      return nil if atom.tag || !atom.classes.empty? || atom.id || !atom.attrs.empty?
      inner_pool_for(inner_arg)
    rescue ArgumentError
      nil
    end

    # Like native_pseudo_data, but rejects any pseudo that requires a
    # nested sub-selector (`:not`/`:is`/`:has`). The C `c_simple_atom`
    # only has the leaf pseudo fields; the recursive ones would need
    # their own inner pool which we don't allocate.
    def self.native_leaf_pseudo_data(pseudos)
      flags = 0
      nth_a = nth_b = 0
      nth_type_a = nth_type_b = 0
      pseudos.each do |name, arg, double_colon|
        return nil if double_colon
        if (bit = NATIVE_PSEUDO_FLAGS[name])
          flags |= bit
        elsif (bit = NATIVE_NTH_BITS[name])
          a, b = Scrapetor::Selector.parse_nth(arg)
          return nil unless a
          flags |= bit
          if name == "nth-of-type" || name == "nth-last-of-type"
            nth_type_a, nth_type_b = a, b
          else
            nth_a, nth_b = a, b
          end
        else
          return nil
        end
      end
      [flags, nth_a, nth_b, nth_type_a, nth_type_b]
    end

    # Split a CSS selector on top-level commas (outside [...] and (...)).
    def self.split_selector_groups(s)
      groups = []
      buf = +""
      depth = 0
      paren = 0
      s.each_char do |ch|
        case ch
        when "[" then depth += 1; buf << ch
        when "]" then depth -= 1 if depth.positive?; buf << ch
        when "(" then paren += 1; buf << ch
        when ")" then paren -= 1 if paren.positive?; buf << ch
        when ","
          if depth.zero? && paren.zero?
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

    # Returns true if the comma-separated selector has groups with
    # different pseudo-element shapes — e.g. `.a > ::text, .b` — so
    # callers can split + peel per-group instead of one shared peel.
    def self.heterogeneous_pseudo_groups?(s)
      groups = split_selector_groups(s)
      kinds = groups.map { |g| peel_pseudo_element(g)[1] }
      kinds.uniq.size > 1
    end

    # `:is(A, B C)`-distribution. Finds a `:is(...)` / `:matches(...)` /
    # `:where(...)` token that sits at an atom boundary (i.e. preceded
    # and followed by start/end/combinator/whitespace) and whose
    # alternatives include at least one with a combinator/whitespace
    # inside. Returns one group string per alternative, with the
    # alternative substituted in. Without this rewrite a selector like
    # `:is(aside, main .x) .y` falls back to the Ruby DOM parser because
    # the native engine can't represent multi-atom alternatives inside
    # `:is`. Returns `[group_str]` (single element) when no rewrite
    # applies — caller treats that as a no-op.
    IS_AT_BOUNDARY_RE = /
      (?:\A|(?<=[\s>+~,]))
      :(?:is|matches|where)\(
    /x.freeze
    def self.expand_is_groups(group_str, force: false)
      m = IS_AT_BOUNDARY_RE.match(group_str)
      return [group_str] unless m
      paren_start = m.end(0) - 1   # position of '('
      depth = 1
      i = paren_start + 1
      len = group_str.length
      while i < len && depth > 0
        ch = group_str[i]
        if ch == "("
          depth += 1
        elsif ch == ")"
          depth -= 1
        end
        i += 1
      end
      return [group_str] if depth != 0
      paren_end = i - 1  # position of matching ')'
      inner = group_str[(paren_start + 1)...paren_end]
      alts = split_selector_groups(inner)
      return [group_str] if alts.size < 2
      # By default only distribute when an alternative has a combinator
      # (multi-atom) — single-atom alternatives compile natively as
      # is_inner. When called from inside `:has`, force distribution so
      # the inner pool sees plain single atoms rather than `:is(...)`
      # wrappers that don't fit native_inner_simples.
      multi = alts.any? { |a| a =~ /[\s>+~]/ }
      return [group_str] unless multi || force
      prefix = group_str[0...m.begin(0)]
      suffix = group_str[(paren_end + 1)..]
      alts.flat_map do |alt|
        merged = "#{prefix}#{alt}#{suffix}".strip
        expand_is_groups(merged, force: force)
      end
    end
  end
end
