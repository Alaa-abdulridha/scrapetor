# frozen_string_literal: true

module Scrapetor
  module Dom
    # CSS selector engine over the pure-Ruby DOM.
    #
    # Pipeline:
    #   1. Compile the selector string into a list of "atoms" with
    #      combinators (reuses `Scrapetor::Selector.compile`).
    #   2. Find candidates matching the rightmost atom by walking the
    #      subtree once (no global indexes — the DOM is small enough
    #      that one scan is faster than maintaining indexes for the
    #      typical scraping document).
    #   3. For each candidate, walk ancestors right-to-left to verify
    #      the rest of the chain.
    #
    # Supports the same selector subset our native engine does (tag,
    # class, id, attribute, descendant, child) — plus comma-separated
    # selector groups.
    module Selectors
      def self.css(scope, selector_str)
        results = []
        seen = {}
        selector_groups(selector_str).each do |group|
          plan = compile(group)
          next if plan.empty?
          execute(scope, plan).each do |n|
            oid = n.object_id
            next if seen[oid]
            seen[oid] = true
            results << n
          end
        end
        results
      end

      def self.selector_groups(s)
        depth = 0
        groups = []
        buf = +""
        s.each_char do |ch|
          if ch == "["
            depth += 1; buf << ch
          elsif ch == "]"
            depth -= 1 if depth.positive?; buf << ch
          elsif ch == "," && depth.zero?
            groups << buf.strip
            buf = +""
          else
            buf << ch
          end
        end
        groups << buf.strip
        groups.reject(&:empty?)
      end

      def self.compile(selector)
        Scrapetor::Selector.compile(selector)
      end

      def self.execute(scope, plan)
        return [] if plan.empty?
        last_idx = plan.size - 1
        candidates = candidates_for_atom(scope, plan[last_idx])
        return candidates if plan.size == 1
        candidates.select { |n| match_chain_backwards?(n, plan, last_idx - 1, scope) }
      end

      def self.candidates_for_atom(scope, atom)
        # Iterate all descendants of scope and filter.
        result = []
        walk_descendants(scope) do |node|
          result << node if atom_matches?(atom, node)
        end
        result
      end

      def self.walk_descendants(scope, &block)
        children =
          if scope.is_a?(Document) || scope.is_a?(Element)
            scope.children
          else
            []
          end
        children.each do |c|
          if c.element?
            block.call(c)
            walk_descendants(c, &block)
          end
        end
      end

      def self.atom_matches?(atom, node)
        return false unless node.element?
        return false if atom.tag && node.name != atom.tag.to_s
        if atom.classes.any?
          cls = node["class"]
          return false if cls.nil?
          cls_set = cls.split(/\s+/)
          atom.classes.each { |c| return false unless cls_set.include?(c) }
        end
        return false if atom.id && node["id"] != atom.id
        atom.attrs.each do |name, op, val|
          v = node[name]
          case op
          when nil  then return false if v.nil?
          when "="  then return false unless v == val
          when "*=" then return false if v.nil? || !v.include?(val)
          when "^=" then return false if v.nil? || !v.start_with?(val)
          when "$=" then return false if v.nil? || !v.end_with?(val)
          when "~=" then return false if v.nil? || !v.split(/\s+/).include?(val)
          when "|=" then return false if v.nil? || (v != val && !v.start_with?("#{val}-"))
          end
        end
        true
      end

      def self.match_chain_backwards?(node, plan, idx, scope)
        return true if idx < 0
        atom = plan[idx]
        combinator = plan[idx + 1].combinator
        case combinator
        when :child
          parent = node.parent
          return false unless parent.is_a?(Element)
          return false unless in_scope?(parent, scope)
          return false unless atom_matches?(atom, parent)
          match_chain_backwards?(parent, plan, idx - 1, scope)
        when :descendant, nil
          cur = node.parent
          while cur.is_a?(Element)
            if in_scope?(cur, scope) && atom_matches?(atom, cur) &&
               match_chain_backwards?(cur, plan, idx - 1, scope)
              return true
            end
            cur = cur.parent
          end
          false
        else
          false
        end
      end

      def self.in_scope?(node, scope)
        return true if scope.is_a?(Document)
        cur = node
        while cur
          return true if cur.equal?(scope)
          cur = cur.parent
        end
        false
      end
    end
  end
end
