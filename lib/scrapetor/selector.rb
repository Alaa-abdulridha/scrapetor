# frozen_string_literal: true

module Scrapetor
  # Selector compiler + executor.
  #
  # The plan is the architectural win that lets Phase 1 beat Nokogiri on
  # repeated-extraction workloads without a native backend: every selector
  # compiles into a list of "atoms" (each `tag.class#id[attr=value]`) plus a
  # combinator linking it to the previous atom. Execution evaluates the
  # *rightmost* atom first, sourcing candidates from structural indexes
  # (O(1) lookup per class/id), then walks ancestor chains backward to
  # verify the rest. This is the same right-to-left strategy real browsers
  # use, applied against indexed candidate sets instead of full-tree walks.
  module Selector
    Atom = Struct.new(:tag, :classes, :id, :attrs, :combinator)

    ATTR_RE = /\A\[([\w:-]+)(?:([*^$~|]?=)["']?([^\]"']*)["']?)?\]/.freeze

    def self.compile(selector)
      sel = selector.to_s.strip
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

    def self.take_atom(s, combinator)
      atom = Atom.new(nil, [], nil, [], combinator)
      scanner = s
      saw_universal = false
      # Tag (or universal *)
      m = scanner.match(/\A([a-zA-Z][\w-]*|\*)/)
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
          m = scanner.match(/\A\.([\w-]+)/) || raise(ArgumentError, "Bad class selector: #{s}")
          atom.classes << m[1]
          scanner = scanner[m[0].size..]
        when "#"
          m = scanner.match(/\A#([\w-]+)/) || raise(ArgumentError, "Bad id selector: #{s}")
          atom.id = m[1]
          scanner = scanner[m[0].size..]
        when "["
          m = scanner.match(ATTR_RE) || raise(ArgumentError, "Bad attribute selector: #{s}")
          atom.attrs << [m[1], m[2], m[3]]
          scanner = scanner[m[0].size..]
        else
          break
        end
      end
      if !saw_universal && atom.tag.nil? && atom.classes.empty? && atom.id.nil? && atom.attrs.empty?
        raise ArgumentError, "Cannot parse selector atom near: #{s}"
      end
      [atom, scanner]
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

    # Execute a compiled plan against a backing scope (Nokolexbor node).
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
      # Pick the narrowest available structural index as the candidate source.
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
        # No primary index — fall back to all elements in scope.
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
        end
      end
      true
    end

    def self.match_chain_backwards?(node, plan, idx, scope)
      return true if idx < 0
      atom = plan[idx]
      # combinator linking atom (idx) to plan[idx+1] is stored on plan[idx+1].combinator
      combinator = plan[idx + 1].combinator
      case combinator
      when :child
        parent = node.parent
        return false if parent.nil?
        return false unless parent.respond_to?(:element?) && parent.element?
        return false unless atom_matches?(atom, parent)
        return false unless in_scope?(parent, scope)
        match_chain_backwards?(parent, plan, idx - 1, scope)
      when :descendant, nil
        cur = node.parent
        while cur && cur.respond_to?(:element?) && cur.element?
          if in_scope?(cur, scope) && atom_matches?(atom, cur) && match_chain_backwards?(cur, plan, idx - 1, scope)
            return true
          end
          cur = cur.parent
        end
        false
      else
        # adj/gen not yet supported in Phase 1
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
