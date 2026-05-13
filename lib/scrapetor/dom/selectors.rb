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
    # Atom matching delegates to `Scrapetor::Selector.atom_matches?`, so
    # pseudo-class support (`:has`, `:not`, `:is`, `:nth-child`, etc.)
    # lives in one place.
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
        paren = 0
        groups = []
        buf = +""
        s.each_char do |ch|
          if ch == "["
            depth += 1; buf << ch
          elsif ch == "]"
            depth -= 1 if depth.positive?; buf << ch
          elsif ch == "("
            paren += 1; buf << ch
          elsif ch == ")"
            paren -= 1 if paren.positive?; buf << ch
          elsif ch == "," && depth.zero? && paren.zero?
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
        result = []
        walk_descendants(scope) do |node|
          result << node if Scrapetor::Selector.atom_matches?(atom, node)
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
        Scrapetor::Selector.atom_matches?(atom, node)
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
        when :adj
          prev = node.previous_element_sibling
          return false unless prev.is_a?(Element)
          return false unless in_scope?(prev, scope)
          return false unless atom_matches?(atom, prev)
          match_chain_backwards?(prev, plan, idx - 1, scope)
        when :gen
          prev = node.previous_element_sibling
          while prev.is_a?(Element)
            if in_scope?(prev, scope) && atom_matches?(atom, prev) &&
               match_chain_backwards?(prev, plan, idx - 1, scope)
              return true
            end
            prev = prev.previous_element_sibling
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
