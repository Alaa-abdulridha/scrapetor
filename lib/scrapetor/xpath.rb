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
  # NOT supported (would need a real XPath 1.0 engine; falls back to
  # raising):
  #   - boolean / and / or
  #   - axes other than child / descendant / parent / self
  #   - numeric comparisons (price > 100)
  #   - union (`|`)
  #   - namespace-prefixed names (xmlns:foo)
  #
  # Honest scope: aimed at the 80% of Nokogiri-migrated XPath that
  # boils down to a structural walk plus simple predicates. For
  # anything beyond, drop to CSS or pre-process the document first.
  module XPath
    class UnsupportedError < StandardError; end

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
    #   @attr
    #   .
    #   ..
    #   tag[pred1][pred2]
    def self.parse_step(s, raw)
      step = { axis: :child, predicates: [] }
      rest = s.dup

      # Strip off predicates first so the node-test has a clean string.
      preds = []
      while (m = rest.match(/\A([^\[]+)\[([^\[\]]+)\](.*)\z/m))
        rest = m[1] + m[3]
        preds << m[2]
      end
      step[:predicates] = preds.map { |p| parse_predicate(p, raw) }

      case rest
      when "."   then step[:node_test] = :self
      when ".."  then step[:axis] = :parent; step[:node_test] = :any
      when "*"   then step[:node_test] = :any_element
      when /\A@([\w\-]+)\z/
        step[:node_test] = { kind: :attr, name: Regexp.last_match(1) }
      when /\Atext\(\)\z/
        step[:node_test] = :text
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
      steps.each_with_index do |st, idx|
        is_last = idx == steps.length - 1
        # `@attr` and `text()` are *terminal* extractions from the
        # current node set — not child-axis traversals. Apply them
        # in place without advancing the axis.
        terminal = st[:node_test].is_a?(Hash) && st[:node_test][:kind] == :attr
        terminal ||= st[:node_test] == :text
        if terminal
          current = filter_by_node_test(current, st[:node_test], is_last)
          # Predicates against attribute / text node-sets are unusual
          # in real-world XPath; apply them only if explicitly set.
          st[:predicates].each { |pr| current = apply_predicate(current, pr) }
          next
        end
        case st[:axis]
        when :root
          current = [root_of(context)]
        when :descendant_or_self
          current = current.flat_map { |n| descendants(n) }
        when :parent
          current = current.map { |n| parent_of(n) }.compact
        when :child
          current = current.flat_map { |n| element_children(n) }
        end
        # Apply node test
        current = filter_by_node_test(current, st[:node_test], is_last)
        # Apply predicates
        st[:predicates].each { |pr| current = apply_predicate(current, pr) }
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

    def self.filter_by_node_test(nodes, test, is_last)
      case test
      when nil, :any
        nodes
      when :any_element
        nodes.select { |n| n.respond_to?(:name) && n.respond_to?(:children) }
      when :text
        # text() — extract text strings from each node.
        nodes.map { |n| n.respond_to?(:text) ? n.text : n.to_s }
      when :self
        nodes
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
