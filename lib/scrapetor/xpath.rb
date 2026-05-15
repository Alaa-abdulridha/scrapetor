# frozen_string_literal: true

module Scrapetor
  # XPath subset for Nokogiri-migration. Implements the most common
  # XPath 1.0 idioms used in real scraping code:
  #
  #   //div                                 - any element by tag (descendant)
  #   //*                                   - any element
  #   /html/body                            - root child path
  #   /html/body/div[1]                     - position predicate
  #   //a/@href                             - attribute access
  #   //div[@class='card']                  - attribute equality
  #   //div[@class]                         - attribute presence
  #   //div[contains(@class,'card')]        - substring
  #   //div[starts-with(@class,'card')]     - prefix
  #   //a[text()='Next']                    - text equality
  #   //a/text()                            - text node access
  #   //div[@id='main']//h2                 - chained descendant
  #   .                                     - context node
  #   ..                                    - parent
  #
  # Axis support (`axis::node_test`):
  #   //dt[text()='Price']/following-sibling::dd
  #   //span[@class='value']/preceding-sibling::span
  #   //a[@id='link']/ancestor::section
  #   //a[@id='link']/ancestor-or-self::*
  #
  # Comment node test:
  #   //comment()                           - every comment in tree
  #   //div/comment()                       - comment children of div
  #
  # NOT supported (would need a real XPath 1.0 engine; falls back to
  # raising):
  #   - boolean / and / or
  #   - axes other than child / descendant / parent / self /
  #     following-sibling / preceding-sibling / ancestor / ancestor-or-self
  #   - numeric comparisons (price > 100)
  #   - union (`|`)
  #   - namespace-prefixed names (xmlns:foo)
  #
  # Honest scope: aimed at the 95% of Nokogiri-migrated XPath that
  # boils down to a structural walk plus simple predicates. For
  # anything beyond, drop to CSS or pre-process the document first.
  module XPath
    class UnsupportedError < StandardError; end

    AXIS_NAMES = {
      "child"             => :child,
      "descendant"        => :descendant,
      "descendant-or-self" => :descendant_or_self,
      "parent"            => :parent,
      "self"              => :self,
      "following-sibling" => :following_sibling,
      "preceding-sibling" => :preceding_sibling,
      "ancestor"          => :ancestor,
      "ancestor-or-self"  => :ancestor_or_self
    }.freeze

    # Top-level entry. Evaluate `expr` against the given Scrapetor
    # document or node, returning an Array of Scrapetor::Node or
    # an Array of String (when the expression ends in /@attr or
    # /text()). Raises UnsupportedError for syntax we don't handle.
    def self.evaluate(context, expr)
      steps = parse(expr.to_s)
      walk(context, steps)
    end

    # --- parser ------------------------------------------------------

    def self.parse(expr)
      raise UnsupportedError, "blank expression" if expr.nil? || expr.empty?
      raise UnsupportedError, "`|` union not supported: #{expr}" if expr.include?("|")
      tokens = tokenize(expr)
      build_steps(tokens, expr)
    end

    # Tokenize an XPath into a flat array of segments. Splits on '/'
    # outside of bracketed predicates. Each segment carries its
    # node-test + zero or more predicates.
    def self.tokenize(expr)
      out = []
      buf = +""
      depth = 0
      expr.each_char do |c|
        if c == "[" then depth += 1; buf << c
        elsif c == "]" then depth -= 1; buf << c
        elsif c == "/" && depth == 0
          out << buf unless buf.empty?
          out << "/"
          buf = +""
        else
          buf << c
        end
      end
      out << buf unless buf.empty?
      out
    end

    def self.build_steps(tokens, raw)
      steps = []
      i = 0
      # Leading "//"  -> descendant-or-self at root.
      # Leading "/"   -> child of root.
      # Leading neither -> relative (descendant by default? we use child of context for safety).
      if tokens[0] == "/" && tokens[1] == "/"
        steps << { axis: :descendant_or_self, node_test: :any_element, predicates: [] }
        i = 2
      elsif tokens[0] == "/"
        # Absolute path from root document.
        steps << { axis: :root, node_test: :any, predicates: [] }
        i = 1
      end

      while i < tokens.length
        sep = tokens[i] == "/" ? tokens[i] : nil
        if sep
          i += 1
          # "//" sequence
          if tokens[i] == "/"
            steps << { axis: :descendant_or_self, node_test: :any_element, predicates: [] }
            i += 1
          end
        end
        break if i >= tokens.length
        step_str = tokens[i]
        i += 1
        steps << parse_step(step_str, raw)
        # If the previous sep was nil, default axis is child.
      end
      steps
    end

    # Parse one step: optional axis, node-test, zero or more predicates.
    #   tag
    #   *
    #   text()
    #   comment()
    #   node()
    #   @attr
    #   .
    #   ..
    #   tag[pred1][pred2]
    #   following-sibling::tag
    #   ancestor::*[predicate]
    def self.parse_step(s, raw)
      step = { axis: :child, predicates: [] }
      rest = s.dup

      # Optional axis prefix: `axis::`. Strip first so the node-test
      # parser doesn't see the `::`.
      if (m = rest.match(/\A([a-zA-Z][a-zA-Z\-]*)::(.*)\z/m))
        axis_name = m[1]
        axis = AXIS_NAMES[axis_name]
        raise UnsupportedError, "unsupported axis `#{axis_name}` in `#{raw}`" unless axis
        step[:axis] = axis
        rest = m[2]
      end

      # Strip off predicates so the node-test has a clean string.
      preds = []
      while (m = rest.match(/\A([^\[]+)\[([^\[\]]+)\](.*)\z/m))
        rest = m[1] + m[3]
        preds << m[2]
      end
      step[:predicates] = preds.map { |p| parse_predicate(p, raw) }

      case rest
      when "."         then step[:node_test] = :self
      when ".."        then step[:axis] = :parent; step[:node_test] = :any
      when "*"         then step[:node_test] = :any_element
      when /\A@([\w\-]+)\z/
        step[:node_test] = { kind: :attr, name: Regexp.last_match(1) }
      when /\Atext\(\)\z/
        step[:node_test] = :text
      when /\Acomment\(\)\z/
        step[:node_test] = :comment
      when /\Anode\(\)\z/
        step[:node_test] = :any_node
      when /\A([a-zA-Z][\w\-]*)\z/
        step[:node_test] = { kind: :tag, name: Regexp.last_match(1) }
      else
        raise UnsupportedError, "can't parse step `#{rest}` in `#{raw}`"
      end
      step
    end

    # Parse a predicate body. We handle:
    #   N                          (numeric position)
    #   @attr
    #   @attr='value'
    #   contains(@attr,'sub')
    #   starts-with(@attr,'pfx')
    #   text()='Next'
    def self.parse_predicate(p, raw)
      p = p.strip
      if p =~ /\A\d+\z/
        return { kind: :position, value: p.to_i }
      end
      if (m = p.match(/\A@([\w\-]+)\s*=\s*['"](.*)['"]\z/m))
        return { kind: :attr_eq, name: m[1], value: m[2] }
      end
      if (m = p.match(/\A@([\w\-]+)\z/))
        return { kind: :attr_present, name: m[1] }
      end
      if (m = p.match(/\Acontains\(\s*@([\w\-]+)\s*,\s*['"](.*)['"]\s*\)\z/m))
        return { kind: :attr_contains, name: m[1], value: m[2] }
      end
      if (m = p.match(/\Astarts-with\(\s*@([\w\-]+)\s*,\s*['"](.*)['"]\s*\)\z/m))
        return { kind: :attr_starts, name: m[1], value: m[2] }
      end
      if (m = p.match(/\Atext\(\)\s*=\s*['"](.*)['"]\z/m))
        return { kind: :text_eq, value: m[1] }
      end
      raise UnsupportedError, "unsupported predicate `#{p}` in `#{raw}`"
    end

    # --- evaluator ---------------------------------------------------

    def self.walk(context, steps)
      # Current set of matching nodes.
      current = [context]
      i = 0
      while i < steps.length
        st = steps[i]
        is_last = i == steps.length - 1

        # `@attr` and `text()` are *terminal* extractions from the
        # current node set — not child-axis traversals. Apply them
        # in place without advancing the axis.
        terminal = st[:node_test].is_a?(Hash) && st[:node_test][:kind] == :attr
        terminal ||= st[:node_test] == :text
        if terminal
          current = filter_by_node_test(current, st[:node_test], is_last)
          st[:predicates].each { |pr| current = apply_predicate(current, pr) }
          i += 1
          next
        end

        # `//comment()` shortcut: a `descendant_or_self any_element`
        # step immediately followed by a `child comment()` step gets
        # collapsed to a single descendant-comments walk. This is the
        # natural collapse of XPath's `descendant-or-self::node()/comment()`
        # rewrite and lets the native engine answer with a single C
        # call instead of walking the full descendant element set.
        if st[:axis] == :descendant_or_self && st[:node_test] == :any_element
          peek = steps[i + 1]
          if peek && peek[:axis] == :child && peek[:node_test] == :comment
            current = current.flat_map { |n| descendant_comments(n) }
            peek[:predicates].each { |pr| current = apply_predicate(current, pr) }
            i += 2
            next
          end
        end

        case st[:axis]
        when :root
          current = [root_of(context)]
        when :descendant_or_self
          current = current.flat_map { |n| descendants(n) }
        when :descendant
          current = current.flat_map { |n| descendants(n) }
        when :parent
          current = current.map { |n| parent_of(n) }.compact
        when :ancestor
          current = current.flat_map { |n| ancestors_of(n) }
        when :ancestor_or_self
          # Ancestors of(n) are root-first (document order); append self
          # so the combined result is in document order.
          current = current.flat_map { |n| [*ancestors_of(n), n] }
        when :following_sibling
          current = current.flat_map { |n| following_siblings_of(n) }
        when :preceding_sibling
          current = current.flat_map { |n| preceding_siblings_of(n) }
        when :self
          # leave current as-is
        when :child
          if st[:node_test] == :comment
            current = current.flat_map { |n| child_comments(n) }
          else
            current = current.flat_map { |n| element_children(n) }
          end
        end

        # `:comment` was already consumed by the child-axis branch above
        # — the filter pass would discard everything since CommentNodes
        # don't respond to :name like an element. Skip the filter when
        # we just produced comment results.
        unless st[:axis] == :child && st[:node_test] == :comment
          current = filter_by_node_test(current, st[:node_test], is_last)
        end
        st[:predicates].each { |pr| current = apply_predicate(current, pr) }
        i += 1
      end
      current
    end

    def self.root_of(ctx)
      n = ctx
      n = n.parent while n.respond_to?(:parent) && n.parent
      n
    end

    def self.descendants(node)
      # Document/node + all descendant elements, in DFS pre-order.
      out = []
      stack = [node]
      while (n = stack.pop)
        case n
        when Scrapetor::Document
          n.css("*").each { |el| out << el }
          return out
        when Scrapetor::Node
          out << n
          # Push children in reverse so DFS produces document order.
          kids = n.children.select { |c| c.respond_to?(:name) }
          stack.concat(kids.reverse)
        end
      end
      out
    end

    def self.element_children(node)
      case node
      when Scrapetor::Document then node.css("> *") rescue node.css("*")
      else node.children.select { |c| c.respond_to?(:name) }
      end
    end

    def self.parent_of(node)
      node.respond_to?(:parent) ? node.parent : nil
    end

    # ---- axis walks (native-fast when the backing supports it) ----

    def self.ancestors_of(node)
      return [] unless node.is_a?(Scrapetor::Node)
      if (ids_and_doc = native_ids_for(node, :node_ancestor_ids))
        nd, ids, wrapper = ids_and_doc
        ids.map { |i| Scrapetor::Node.new(node.document, Scrapetor::Native::Element.new(nd, i, wrapper)) }
      else
        list = []
        cur = node.parent
        while cur
          list << cur
          cur = cur.parent
        end
        list.reverse
      end
    end

    def self.following_siblings_of(node)
      return [] unless node.is_a?(Scrapetor::Node)
      if (ids_and_doc = native_ids_for(node, :node_following_sibling_ids))
        nd, ids, wrapper = ids_and_doc
        ids.map { |i| Scrapetor::Node.new(node.document, Scrapetor::Native::Element.new(nd, i, wrapper)) }
      else
        list = []
        cur = node.next_sibling
        while cur
          list << cur if cur.respond_to?(:element?) && cur.element?
          cur = cur.respond_to?(:next_sibling) ? cur.next_sibling : nil
        end
        list
      end
    end

    def self.preceding_siblings_of(node)
      return [] unless node.is_a?(Scrapetor::Node)
      if (ids_and_doc = native_ids_for(node, :node_preceding_sibling_ids))
        nd, ids, wrapper = ids_and_doc
        ids.map { |i| Scrapetor::Node.new(node.document, Scrapetor::Native::Element.new(nd, i, wrapper)) }
      else
        list = []
        cur = node.previous_sibling
        while cur
          list.unshift(cur) if cur.respond_to?(:element?) && cur.element?
          cur = cur.respond_to?(:previous_sibling) ? cur.previous_sibling : nil
        end
        list
      end
    end

    def self.child_comments(node)
      nd, root_id, _ = native_doc_for(node)
      if nd
        nd.node_child_comment_ids(root_id).map { |i| Scrapetor::CommentNode.new(document_of(node), nd.node_comment_text(i)) }
      elsif node.is_a?(Scrapetor::Node)
        node.backing_node.children.to_a.select { |c| c.respond_to?(:comment?) && c.comment? }
            .map { |c| Scrapetor::CommentNode.new(node.document, c) }
      elsif node.is_a?(Scrapetor::Document)
        bk = node.backing
        return [] unless bk.respond_to?(:fallback_dom) || bk.respond_to?(:children)
        kids = bk.respond_to?(:fallback_dom) ? bk.fallback_dom.children : bk.children
        kids.to_a.select { |c| c.respond_to?(:comment?) && c.comment? }
            .map { |c| Scrapetor::CommentNode.new(node, c) }
      else
        []
      end
    end

    def self.descendant_comments(node)
      nd, root_id, _ = native_doc_for(node)
      if nd
        nd.node_descendant_comment_ids(root_id).map { |i| Scrapetor::CommentNode.new(document_of(node), nd.node_comment_text(i)) }
      elsif node.is_a?(Scrapetor::Node)
        out = []
        walk_comments_ruby(node.backing_node, out, node.document)
        out
      elsif node.is_a?(Scrapetor::Document)
        bk = node.backing
        root = bk.respond_to?(:fallback_dom) ? bk.fallback_dom : bk
        out = []
        walk_comments_ruby(root, out, node)
        out
      else
        []
      end
    end

    # Returns [native_doc, root_id, wrapper] when the context can be
    # answered by the C arena (Native::Element or Native-backed
    # Document). For Document context, root_id is 0 (the doc node) so
    # callers see every comment in the tree, including any that sit
    # outside <html>.
    def self.native_doc_for(node)
      if node.is_a?(Scrapetor::Document)
        bk = node.backing
        if defined?(Scrapetor::Native::DocumentWrapper) && bk.is_a?(Scrapetor::Native::DocumentWrapper) &&
           bk.native.respond_to?(:node_descendant_comment_ids)
          return [bk.native, 0, bk]
        end
      elsif node.is_a?(Scrapetor::Node)
        bk = node.backing_node
        if bk.respond_to?(:id) && bk.respond_to?(:doc) && bk.doc.respond_to?(:node_descendant_comment_ids)
          wrapper = bk.respond_to?(:wrapper) ? bk.wrapper : nil
          return [bk.doc, bk.id, wrapper]
        end
      end
      nil
    end

    # Returns [native_doc, ids, wrapper] for a Node whose backing
    # exposes `method_name` on its native_doc; nil otherwise. Lets the
    # axis walks short-circuit to a single C call.
    def self.native_ids_for(node, method_name)
      bk = node.backing_node
      return nil unless bk.respond_to?(:id) && bk.respond_to?(:doc) && bk.doc.respond_to?(method_name)
      wrapper = bk.respond_to?(:wrapper) ? bk.wrapper : nil
      [bk.doc, bk.doc.public_send(method_name, bk.id), wrapper]
    end

    def self.document_of(node)
      node.is_a?(Scrapetor::Document) ? node : node.document
    end

    def self.walk_comments_ruby(backing, out, doc)
      return unless backing.respond_to?(:children)
      backing.children.to_a.each do |c|
        if c.respond_to?(:comment?) && c.comment?
          out << Scrapetor::CommentNode.new(doc, c)
        elsif c.respond_to?(:children)
          walk_comments_ruby(c, out, doc)
        end
      end
    end

    def self.filter_by_node_test(nodes, test, is_last)
      case test
      when nil, :any, :any_node
        nodes
      when :any_element
        nodes.select { |n| n.respond_to?(:name) && n.respond_to?(:children) }
      when :text
        # text() — extract text strings from each node.
        nodes.map { |n| n.respond_to?(:text) ? n.text : n.to_s }
      when :self
        nodes
      when :comment
        nodes.select { |n| n.respond_to?(:comment?) && n.comment? }
      when Hash
        case test[:kind]
        when :tag
          name = test[:name]
          nodes.select { |n| n.respond_to?(:name) && n.name.casecmp(name).zero? }
        when :attr
          attr = test[:name]
          nodes.map { |n| n[attr] || n["@#{attr}"] }.compact
        end
      end
    end

    def self.apply_predicate(nodes, pred)
      # text-node ARRAYS at this point are already Strings; predicates
      # don't apply to them in our subset.
      return nodes if nodes.any? { |n| n.is_a?(String) }
      case pred[:kind]
      when :position
        idx = pred[:value]
        nodes[(idx - 1)..(idx - 1)] || []
      when :attr_present
        nodes.select { |n| !(v = n[pred[:name]]).nil? && !v.to_s.empty? }
      when :attr_eq
        nodes.select { |n| n[pred[:name]].to_s == pred[:value] }
      when :attr_contains
        nodes.select { |n| n[pred[:name]].to_s.include?(pred[:value]) }
      when :attr_starts
        nodes.select { |n| n[pred[:name]].to_s.start_with?(pred[:value]) }
      when :text_eq
        nodes.select { |n| n.respond_to?(:text) && n.text.to_s.strip == pred[:value] }
      end
    end
  end
end
