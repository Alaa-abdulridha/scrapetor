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
      if pe.casecmp("::text").zero?
        [head, :text, nil]
      elsif (a = pe.match(/::attr\(([^)]+)\)/i))
        [head, :attr, a[1].strip]
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

        AttrNode = Struct.new(:name, :value)

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
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          if kind && !dom_node?
            w = wrapper
            plan = w ? w.compiled_plan(stripped) : Native.compile_selector_chain(stripped)
            if plan && !stripped.include?(",")
              ids = @doc.run_chain(plan, @id)
              return case kind
                     when :text, :text_approx then Native.wrap_text_nodes!(@doc.bulk_text(ids))
                     when :attr               then Native.wrap_text_nodes!(@doc.bulk_attr(ids, arg))
                     end
            end
          end
          nodes = css_native_or_fallback(stripped)
          apply_pseudo_element(nodes, kind, arg)
        end

        def at_css(selector)
          str = selector.to_s
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
            nodes.map { |n| Scrapetor::TextNode.new(n.respond_to?(:text) ? n.text.to_s : n.to_s) }
          when :attr
            nodes.map { |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              v.nil? ? nil : Scrapetor::TextNode.new(v)
            }
          end
        end

        private

        def dom_node?
          !@dom_node.nil?
        end

        # Promote this Element (and the underlying document) to the
        # Ruby DOM. After this, all reads and writes hit @dom_node and
        # the wrapper's @dom_doc rather than the native arena.
        #
        # Two-stage lookup. Strict path-based first (handles well-formed
        # HTML where both parsers produce the same element tree); falls
        # through to DFS pre-order index when the parsers disagree on
        # whitespace or implicit closing — both walk elements in the
        # same order even when their text-node treatment diverges.
        def ensure_dom!
          return @dom_node if @dom_node
          w = wrapper
          raise NotImplementedError, "Mutation requires a DocumentWrapper" if w.nil?
          w.switch_to_dom!
          @dom_node = w.locate_in_dom(path) || w.locate_dom_by_native_id(@id)
          raise NotImplementedError, "Cannot locate equivalent node in fallback DOM" if @dom_node.nil?
          @dom_node
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
          Native.split_selector_groups(selector_str).each do |g|
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
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          if kind && !@dom_mode
            ids = native_ids(stripped)
            if ids
              return case kind
                     when :text, :text_approx then Native.wrap_text_nodes!(@native.bulk_text(ids))
                     when :attr               then Native.wrap_text_nodes!(@native.bulk_attr(ids, arg))
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

        def css(selector)
          str = selector.to_s
          stripped, kind, arg = Native.peel_pseudo_element(str)
          stripped = "*" if stripped.empty?
          if kind && !@dom_mode
            ids = native_ids(stripped)
            if ids
              return case kind
                     when :text, :text_approx then Native.wrap_text_nodes!(@native.bulk_text(ids))
                     when :attr               then Native.wrap_text_nodes!(@native.bulk_attr(ids, arg))
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
                          when :text, :text_approx then Native.wrap_text_nodes!(@native.bulk_text(ids))
                          when :attr               then Native.wrap_text_nodes!(@native.bulk_attr(ids, args[orig]))
                          else                          LazyIds.new(self, @native, ids)
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
            return nil unless plan
            return @native.run_chain(plan, nil)
          end
          ids = []
          seen = nil
          Native.split_selector_groups(selector_str).each do |g|
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
            nodes.map { |n| Scrapetor::TextNode.new(n.respond_to?(:text) ? n.text.to_s : n.to_s) }
          when :attr
            nodes.map { |n|
              v = n.respond_to?(:[]) ? n[arg] : nil
              v.nil? ? nil : Scrapetor::TextNode.new(v)
            }
          end
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
            # Not natively supported — route to Dom fallback.
            list = fallback_dom.css(selector_str).to_a
            list = list.first(1) if limit_one
            return list.map { |n| wrap_dom_node(n) }
          end

          # Comma-separated groups.
          all = []
          seen = nil
          ok = true
          Native.split_selector_groups(selector_str).each do |g|
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
        return nil if atom.combinator == :adj || atom.combinator == :gen
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
          inner = native_inner_simples(arg)
          return NATIVE_PSEUDO_FALLBACK if inner == NATIVE_PSEUDO_FALLBACK
          has_inner.concat(inner)
          flags |= (1 << 22)
        else
          return NATIVE_PSEUDO_FALLBACK
        end
      end

      [flags, nth_a, nth_b, nth_type_a, nth_type_b, not_inner, is_inner, has_inner]
    end

    # Compile an inner-selector argument (`:not(.x, :empty, .y[z])`) into
    # an array of simple-atom descriptors the C engine can read. Each
    # inner is `[tag, classes, id, attrs]` or, when pseudo flags are
    # present, `[tag, classes, id, attrs, leaf_pseudo_data]`. Combinators
    # and recursive pseudos (a `:not` inside a `:not`) still force the
    # Ruby fallback — the C side only flattens one level deep.
    def self.native_inner_simples(arg)
      return NATIVE_PSEUDO_FALLBACK if arg.nil? || arg.empty?
      groups = Scrapetor::Dom::Selectors.selector_groups(arg)
      out = []
      groups.each do |g|
        plan = Scrapetor::Selector.compile(g)
        return NATIVE_PSEUDO_FALLBACK if plan.size != 1
        atom = plan.first
        leaf_pseudo = nil
        if atom.pseudos && !atom.pseudos.empty?
          leaf_pseudo = native_leaf_pseudo_data(atom.pseudos)
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
  end
end
