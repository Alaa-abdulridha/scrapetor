# frozen_string_literal: true

module Scrapetor
  # Selector compiler + executor.
  #
  # The plan is the architectural win that lets Phase 1 beat Nokogiri on
  # repeated-extraction workloads without a native backend: every selector
  # compiles into a list of "atoms" (each `tag.class#id[attr=value]` plus
  # optional pseudo-classes) and a combinator linking it to the previous
  # atom. Execution evaluates the rightmost atom first, sources candidates
  # from structural indexes (O(1) per class/id), then walks ancestor
  # chains backward to verify the rest.
  module Selector
    Atom = Struct.new(:tag, :classes, :id, :attrs, :combinator, :pseudos)

    # Attribute selector: `[name]`, `[name=value]`, `[name*='v']`, etc.
    # The value is captured in one of three slots depending on the quote
    # style so an attribute value like `[class*="L'appareil"]` (single
    # quote inside double-quoted) parses cleanly — the older `[^"']*`
    # value class excluded both quotes and broke on every apostrophe in
    # a double-quoted value, taking out an eBay product fixture.
    #   m[1]: attribute name (Unicode-aware)
    #   m[2]: operator (= / *= / ^= / $= / ~= / |=)
    #   m[3]: value inside double quotes (allows ', escaped chars)
    #   m[4]: value inside single quotes (allows ", escaped chars)
    #   m[5]: bare unquoted value
    ATTR_RE = /
      \A\[
        ([\w:\-\u{0080}-\u{10FFFF}]+)
        (?:
          ([*^$~|]?=)
          (?:
            "((?:[^"\\]|\\.)*)"
          | '((?:[^'\\]|\\.)*)'
          | ([^\]\s]+)
          )
        )?
      \]
    /x.freeze
    PSEUDO_NAME_RE = /\A([a-zA-Z][\w-]*)/.freeze

    # Pseudo-classes Scrapetor can evaluate on a node. Pseudo-elements
    # (`::text`, `::attr(name)`) live on the atom too but are post-
    # processed at the css() boundary, not used for matching.
    KNOWN_PSEUDO_CLASSES = %w[
      not has is matches where
      first-child last-child only-child
      first-of-type last-of-type only-of-type
      nth-child nth-last-child nth-of-type nth-last-of-type
      empty root scope
      checked disabled enabled
      any-link link visited target focus hover active
      required optional read-only read-write placeholder-shown
    ].freeze

    # Pseudo-elements we recognise (Scrapy/Parsel-style). Stored on the
    # last atom; consumed by the public css() entry points.
    KNOWN_PSEUDO_ELEMENTS = %w[text attr first-letter first-line before after].freeze

    def self.compile(selector)
      sel = selector.to_s.strip
      # CSS Selectors Level 4 scope-relative selector: a leading `>`/`+`/`~`
      # is shorthand for `:scope <combinator> rest`. Production code (Scrapy,
      # Parsel, jQuery, real-world scraping parsers) leans on this when
      # calling `node.css("> .child")` or `:has(> .x)`. We desugar it here so
      # the rest of the compiler stays single-shape.
      if !sel.empty? && (sel[0] == ">" || sel[0] == "+" || sel[0] == "~")
        sel = ":scope " + sel
      end
      atoms = []
      remainder = sel
      combinator = nil
      until remainder.empty?
        atom, rest = take_atom(remainder, combinator)
        atoms << atom
        remainder = rest
        break if remainder.empty?
        combinator, remainder = take_combinator(remainder)
      end
      raise ArgumentError, "Empty selector" if atoms.empty?
      atoms
    end

    # Identifier characters. CSS Selectors Level 3 §10.1 allows non-ASCII
    # (>= U+00A0) in identifiers in addition to [a-zA-Z0-9_-]. Real-world
    # class names like `caractéristiquesPrincipalesDuProduit` (eBay FR)
    # or Cyrillic/CJK class names need the Unicode-aware character set
    # — `\w` on its own matches ASCII only.
    IDENT_TAG_RE   = /\A([a-zA-Z][\w\-\u{0080}-\u{10FFFF}]*|\*)/.freeze
    IDENT_CLASS_RE = /\A\.([\w\-\u{0080}-\u{10FFFF}]+)/.freeze
    IDENT_ID_RE    = /\A#([\w\-\u{0080}-\u{10FFFF}]+)/.freeze

    def self.take_atom(s, combinator)
      atom = Atom.new(nil, [], nil, [], combinator, nil)
      scanner = s
      saw_universal = false
      m = scanner.match(IDENT_TAG_RE)
      if m
        tag = m[1]
        if tag == "*"
          saw_universal = true
        else
          atom.tag = tag.downcase.to_sym
        end
        scanner = scanner[m[0].size..]
      end
      loop do
        case scanner[0]
        when "."
          m = scanner.match(IDENT_CLASS_RE) || raise(ArgumentError, "Bad class selector: #{s}")
          atom.classes << m[1]
          scanner = scanner[m[0].size..]
        when "#"
          m = scanner.match(IDENT_ID_RE) || raise(ArgumentError, "Bad id selector: #{s}")
          atom.id = m[1]
          scanner = scanner[m[0].size..]
        when "["
          m = scanner.match(ATTR_RE) || raise(ArgumentError, "Bad attribute selector: #{s}")
          # m[3] = double-quoted value, m[4] = single-quoted, m[5] = bare.
          # Whichever capture matched is the actual value; the others are
          # nil. The unquoted slot is `[^\]\s]+`, so values with embedded
          # whitespace must be quoted — same as the CSS Selectors Level 3
          # grammar requires.
          val = m[3] || m[4] || m[5]
          atom.attrs << [m[1], m[2], val]
          scanner = scanner[m[0].size..]
        when ":"
          name, arg, double_colon, rest = take_pseudo(scanner)
          atom.pseudos ||= []
          atom.pseudos << [name, arg, double_colon]
          scanner = rest
        else
          break
        end
      end
      empty = !saw_universal &&
              atom.tag.nil? &&
              atom.classes.empty? &&
              atom.id.nil? &&
              atom.attrs.empty? &&
              (atom.pseudos.nil? || atom.pseudos.empty?)
      raise ArgumentError, "Cannot parse selector atom near: #{s}" if empty
      [atom, scanner]
    end

    # Consume a pseudo-class (`:name`) or pseudo-element (`::name`),
    # optionally followed by a parenthesised argument. Balanced-paren
    # matching so `:has(div:not(.x))` etc. parse cleanly.
    def self.take_pseudo(s)
      double_colon = s.start_with?("::")
      tail = s[(double_colon ? 2 : 1)..]
      m = tail.match(PSEUDO_NAME_RE) || raise(ArgumentError, "Bad pseudo: #{s}")
      name = m[1].downcase
      tail = tail[m[0].size..]
      arg = nil
      if tail.start_with?("(")
        depth = 1
        i = 1
        len = tail.length
        while i < len && depth > 0
          ch = tail[i]
          depth += 1 if ch == "("
          depth -= 1 if ch == ")"
          i += 1
        end
        raise ArgumentError, "Unterminated pseudo arg: #{s}" if depth > 0
        arg = tail[1...(i - 1)]
        tail = tail[i..]
      end
      [name, arg, double_colon, tail]
    end

    def self.take_combinator(s)
      had_ws = false
      while !s.empty? && (s[0] == " " || s[0] == "\t" || s[0] == "\n")
        had_ws = true
        s = s[1..]
      end
      return [nil, ""] if s.empty?
      case s[0]
      when ">"
        s = s[1..]
        while !s.empty? && (s[0] == " " || s[0] == "\t" || s[0] == "\n")
          s = s[1..]
        end
        [:child, s]
      when "+"
        s = s[1..]
        while !s.empty? && (s[0] == " " || s[0] == "\t" || s[0] == "\n")
          s = s[1..]
        end
        [:adj, s]
      when "~"
        s = s[1..]
        while !s.empty? && (s[0] == " " || s[0] == "\t" || s[0] == "\n")
          s = s[1..]
        end
        [:gen, s]
      else
        if had_ws
          [:descendant, s]
        else
          raise ArgumentError, "Cannot parse combinator near: #{s}"
        end
      end
    end

    # Parse an `an+b` formula used by :nth-child(...) and friends.
    # Returns [a, b] or nil if the argument can't be parsed.
    def self.parse_nth(arg)
      return nil if arg.nil?
      s = arg.to_s.strip.downcase.gsub(/\s+/, "")
      return nil if s.empty?
      return [2, 1] if s == "odd"
      return [2, 0] if s == "even"
      if (m = s.match(/\A([+-]?\d+)\z/))
        return [0, m[1].to_i]
      end
      if (m = s.match(/\A([+-]?\d*)n([+-]\d+)?\z/))
        a_str = m[1]
        a = case a_str
            when "", "+" then 1
            when "-" then -1
            else a_str.to_i
            end
        b = m[2] ? m[2].to_i : 0
        return [a, b]
      end
      nil
    end

    # Given coefficients (a, b) and a 1-based position idx, true if
    # there is a non-negative integer k with idx == a*k + b.
    def self.nth_matches?(a, b, idx)
      return idx == b if a.zero?
      diff = idx - b
      return false if a.positive? && diff.negative?
      return false if a.negative? && diff.positive?
      (diff % a).zero?
    end

    # ----- Execution (used by the Dom::Document selector path). -----

    def self.execute(doc, plan, scope)
      return [] if plan.empty?
      last_idx = plan.size - 1
      candidates = candidates_for_atom(doc, plan[last_idx], scope)
      return candidates if plan.size == 1
      candidates.select do |node|
        match_chain_backwards?(node, plan, last_idx - 1, scope)
      end
    end

    def self.candidates_for_atom(doc, atom, scope)
      sets = []
      if atom.id
        n = doc.id_index[atom.id]
        return [] if n.nil?
        return [] unless in_scope?(n, scope)
        return [n] if atom_matches?(atom, n)
        return []
      end
      atom.classes.each do |c|
        sets << (doc.class_index[c] || [])
      end
      sets << (doc.tag_index[atom.tag] || []) if atom.tag
      candidates = if sets.empty?
        if defined?(Dom::Document) && scope.is_a?(Dom::Document)
          doc.all_elements
        else
          scope.css("*").to_a
        end
      else
        sets.min_by(&:size)
      end
      candidates.select do |n|
        atom_matches?(atom, n) && in_scope?(n, scope)
      end
    end

    def self.atom_matches?(atom, node)
      return false unless node.respond_to?(:element?) && node.element?
      return false if atom.tag && node.name.to_sym != atom.tag
      if atom.classes.any?
        nc = node["class"]
        return false if nc.nil?
        ncs = nc.split(/\s+/)
        atom.classes.each { |c| return false unless ncs.include?(c) }
      end
      return false if atom.id && node["id"] != atom.id
      atom.attrs.each do |name, op, val|
        v = node[name]
        case op
        when nil
          return false if v.nil?
        when "="
          return false unless v == val
        when "*="
          return false if v.nil? || !v.include?(val)
        when "^="
          return false if v.nil? || !v.start_with?(val)
        when "$="
          return false if v.nil? || !v.end_with?(val)
        when "~="
          return false if v.nil? || !v.split(/\s+/).include?(val)
        when "|="
          return false if v.nil? || (v != val && !v.start_with?("#{val}-"))
        end
      end
      if atom.pseudos && !atom.pseudos.empty?
        atom.pseudos.each do |name, arg, double_colon|
          next if double_colon # pseudo-elements aren't matchers
          return false unless pseudo_matches?(node, name, arg)
        end
      end
      true
    end

    def self.pseudo_matches?(node, name, arg)
      case name
      when "not"
        return true if arg.nil? || arg.empty?
        # Any sub-selector matching node disqualifies it.
        Scrapetor::Dom::Selectors.selector_groups(arg).each do |g|
          plan = compile(g)
          return false if matches_chain_at_node?(node, plan)
        end
        true
      when "is", "matches", "where"
        return false if arg.nil? || arg.empty?
        Scrapetor::Dom::Selectors.selector_groups(arg).any? do |g|
          plan = compile(g)
          matches_chain_at_node?(node, plan)
        end
      when "has"
        return false if arg.nil? || arg.empty?
        has_descendant_matching?(node, arg)
      when "first-child"
        node.respond_to?(:previous_element_sibling) && node.previous_element_sibling.nil?
      when "last-child"
        node.respond_to?(:next_element_sibling) && node.next_element_sibling.nil?
      when "only-child"
        node.respond_to?(:next_element_sibling) &&
          node.previous_element_sibling.nil? && node.next_element_sibling.nil?
      when "first-of-type"
        first_of_type?(node)
      when "last-of-type"
        last_of_type?(node)
      when "only-of-type"
        first_of_type?(node) && last_of_type?(node)
      when "nth-child"
        nth_position_match?(node, arg, by_type: false, reverse: false)
      when "nth-last-child"
        nth_position_match?(node, arg, by_type: false, reverse: true)
      when "nth-of-type"
        nth_position_match?(node, arg, by_type: true, reverse: false)
      when "nth-last-of-type"
        nth_position_match?(node, arg, by_type: true, reverse: true)
      when "empty"
        if node.respond_to?(:children)
          node.children.none? { |c| c.element? || (c.respond_to?(:text?) && c.text? && !c.text.to_s.empty?) }
        else
          true
        end
      when "root"
        p = node.respond_to?(:parent) ? node.parent : nil
        p.nil? || (defined?(Scrapetor::Dom::Document) && p.is_a?(Scrapetor::Dom::Document))
      when "scope"
        true
      when "checked"
        truthy_attr?(node, "checked")
      when "disabled"
        truthy_attr?(node, "disabled")
      when "enabled"
        !truthy_attr?(node, "disabled")
      when "required"
        truthy_attr?(node, "required")
      when "optional"
        !truthy_attr?(node, "required")
      when "read-only"
        truthy_attr?(node, "readonly")
      when "read-write"
        !truthy_attr?(node, "readonly")
      when "any-link", "link"
        (node.name == "a" || node.name == "area") && !node["href"].nil?
      else
        # Unknown pseudo-class: fail closed (don't match) so the user
        # sees a missing-result rather than a silent wrong-result.
        false
      end
    end

    def self.truthy_attr?(node, name)
      v = node[name]
      !v.nil? && v != "false"
    end

    def self.first_of_type?(node)
      return true unless node.respond_to?(:previous_sibling)
      sib = node.previous_sibling
      while sib
        return false if sib.respond_to?(:element?) && sib.element? && sib.name == node.name
        sib = sib.previous_sibling
      end
      true
    end

    def self.last_of_type?(node)
      return true unless node.respond_to?(:next_sibling)
      sib = node.next_sibling
      while sib
        return false if sib.respond_to?(:element?) && sib.element? && sib.name == node.name
        sib = sib.next_sibling
      end
      true
    end

    def self.nth_position_match?(node, arg, by_type:, reverse:)
      formula = parse_nth(arg)
      return false unless formula
      parent = node.respond_to?(:parent) ? node.parent : nil
      return false if parent.nil?
      sibs =
        if parent.respond_to?(:children)
          parent.children.select { |c| c.respond_to?(:element?) && c.element? }
        else
          []
        end
      sibs = sibs.select { |s| s.name == node.name } if by_type
      sibs = sibs.reverse if reverse
      idx = sibs.index { |s| s.equal?(node) }
      return false unless idx
      nth_matches?(formula[0], formula[1], idx + 1)
    end

    # Does the node itself match the entire chain (treat the rightmost
    # atom as the anchor, walk back from there). Used by :not, :is, :has.
    def self.matches_chain_at_node?(node, plan)
      return false if plan.empty?
      last_idx = plan.size - 1
      return false unless atom_matches?(plan[last_idx], node)
      return true if plan.size == 1
      match_chain_backwards?(node, plan, last_idx - 1, nil)
    end

    def self.has_descendant_matching?(node, selector_str)
      groups = Scrapetor::Dom::Selectors.selector_groups(selector_str)
      groups.each do |raw_group|
        g = raw_group.strip
        # Scope-relative inner: `:has(> .child)` / `:has(+ .x)` / `:has(~ .x)`.
        # Honour the combinator directly instead of compiling against a
        # synthetic scope atom — that's both more accurate (matches CSS
        # spec) and dodges the "Cannot parse selector atom near: > ..."
        # crash that took out four production scrape fixtures.
        if g.start_with?(">")
          inner = g[1..].lstrip
          plan = compile(inner)
          node.children.each do |c|
            next unless c.respond_to?(:element?) && c.element?
            return true if matches_chain_at_node?(c, plan)
          end
          next
        elsif g.start_with?("+")
          inner = g[1..].lstrip
          plan = compile(inner)
          sib = node.respond_to?(:next_element_sibling) ? node.next_element_sibling : nil
          return true if sib && matches_chain_at_node?(sib, plan)
          next
        elsif g.start_with?("~")
          inner = g[1..].lstrip
          plan = compile(inner)
          sib = node.respond_to?(:next_element_sibling) ? node.next_element_sibling : nil
          while sib
            return true if matches_chain_at_node?(sib, plan)
            sib = sib.next_element_sibling
          end
          next
        end
        plan = compile(g)
        walk_descendants(node) do |d|
          return true if matches_chain_at_node?(d, plan)
        end
      end
      false
    end

    def self.walk_descendants(node, &block)
      return unless node.respond_to?(:children)
      node.children.each do |c|
        next unless c.respond_to?(:element?) && c.element?
        yield c
        walk_descendants(c, &block)
      end
    end

    def self.match_chain_backwards?(node, plan, idx, scope)
      return true if idx < 0
      atom = plan[idx]
      combinator = plan[idx + 1].combinator
      case combinator
      when :child
        parent = node.parent
        return false if parent.nil?
        return false unless parent.respond_to?(:element?) && parent.element?
        return false unless in_scope?(parent, scope)
        return false unless atom_matches?(atom, parent)
        match_chain_backwards?(parent, plan, idx - 1, scope)
      when :descendant, nil
        cur = node.parent
        while cur && cur.respond_to?(:element?) && cur.element?
          if in_scope?(cur, scope) && atom_matches?(atom, cur) &&
             match_chain_backwards?(cur, plan, idx - 1, scope)
            return true
          end
          cur = cur.parent
        end
        false
      when :adj
        prev = node.respond_to?(:previous_element_sibling) ? node.previous_element_sibling : nil
        return false if prev.nil?
        return false unless in_scope?(prev, scope)
        return false unless atom_matches?(atom, prev)
        match_chain_backwards?(prev, plan, idx - 1, scope)
      when :gen
        prev = node.respond_to?(:previous_element_sibling) ? node.previous_element_sibling : nil
        while prev
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
      return true if scope.nil?
      return true if defined?(Dom::Document) && scope.is_a?(Dom::Document)
      cur = node
      while cur
        return true if cur == scope
        cur = cur.parent
      end
      false
    end
  end
end
