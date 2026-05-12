# frozen_string_literal: true

module Scrapetor
  # Bridge to the native streaming extraction engine.
  #
  # If the C extension is loadable, Native.available? is true and
  # Native.compile_descriptor turns a Schema into the flat format the
  # C side consumes. Schemas using features outside the native
  # fast-path subset (combinators, pseudo-classes, nested repeated
  # groups, top-level fields without a repeated context) compile to
  # nil, and the Extractor falls back to the Ruby path.
  module Native
    begin
      require "scrapetor/scrapetor_native"
      AVAILABLE  = true
      LOAD_ERROR = nil
    rescue LoadError => e
      AVAILABLE  = false
      LOAD_ERROR = e
    end

    def self.available?
      AVAILABLE
    end

    # Compile a Schema into the descriptor format the C side consumes.
    #
    #   desc   = [group, group, ...]
    #   group  = [name_sym, sel, fields_array]
    #   field  = [name_sym, sel, attr_str_or_nil, type_sym, clean_bool,
    #             normalize_url_bool, multi_bool]
    #   sel    = [tag_or_nil, classes_array, id_or_nil, attrs_array]
    #   attrs_array = [[name_str, op_str_or_nil, val_str_or_nil], ...]
    #
    # Returns nil if the schema uses features the native path doesn't
    # support yet.
    SYNTHETIC_ROOT = :__scrapetor_root__
    HTML_ROOT_SEL  = ["html", [], nil, []].freeze

    # Memoised on the Schema instance — the descriptor Array tree is
    # identical for every call against the same schema, so rebuilding
    # it on each extract was just GC pressure. Both successful
    # descriptors and the "can't compile" outcome are cached.
    def self.compile_descriptor(schema)
      cached = schema.instance_variable_get(:@__scrapetor_native_desc)
      unless cached.nil?
        return cached == false ? nil : cached
      end

      desc = build_descriptor(schema)
      schema.instance_variable_set(:@__scrapetor_native_desc, desc.nil? ? false : desc)
      desc
    end

    def self.build_descriptor(schema)
      groups = []

      # Top-level fields become a synthetic group bound to the <html>
      # element. The Document layer unwraps the single result back into
      # the top of the response hash. Fragments without <html> fall back
      # to the Ruby path.
      if schema.fields.any?
        field_descs = schema.fields.map { |f| compile_field(f) }
        return nil if field_descs.any?(&:nil?)
        groups << [SYNTHETIC_ROOT, HTML_ROOT_SEL, field_descs]
      end

      schema.groups.each do |g|
        gd = compile_group(g)
        return nil unless gd
        groups << gd
      end

      return nil if groups.empty?
      groups
    end

    def self.compile_group(group)
      sel = parse_selector(group.selector)
      return nil unless sel
      return nil unless group.groups.empty? # nested groups: Ruby fallback
      fields = []
      group.fields.each do |f|
        fd = compile_field(f)
        return nil unless fd
        fields << fd
      end
      [group.name, sel, fields]
    end

    def self.compile_field(field)
      # Features the native engine doesn't yet support — fall back to Ruby.
      return nil if field.selector.is_a?(Array)
      return nil if field.transform
      return nil unless field.default.nil?
      return nil if field.required
      return nil if %i[html list json boolean].include?(field.type)

      # Try simple selector first.
      simple = parse_selector(field.selector)
      if simple
        return [field.name, simple, field.attr_str, field.type,
                !!field.clean, !!field.normalize_url, !!field.multi,
                nil, nil]
      end

      # Try combinator selector.
      combo = parse_selector_with_combinator(field.selector)
      if combo
        primary, combinator, context = combo
        return [field.name, primary, field.attr_str, field.type,
                !!field.clean, !!field.normalize_url, !!field.multi,
                context, combinator]
      end

      nil
    end

    # Parse a CSS selector with at most one combinator (`A B` or `A > B`).
    # Returns [primary_sel, combinator_str, context_sel] or nil if the
    # input has multiple combinators or other unsupported syntax.
    def self.parse_selector_with_combinator(selector)
      s = selector.to_s.strip
      return nil if s.empty?

      # Split on first combinator at top level (outside [...] groups).
      split = split_at_combinator(s)
      return nil unless split
      left_str, combinator, right_str = split

      left  = parse_selector(left_str)
      right = parse_selector(right_str)
      return nil unless left && right

      [right, combinator, left]
    end

    def self.split_at_combinator(s)
      depth = 0
      i = 0
      while i < s.length
        ch = s[i]
        if ch == "["
          depth += 1
        elsif ch == "]"
          depth -= 1 if depth.positive?
        elsif depth.zero?
          if ch == ">"
            left = s[0...i].strip
            right = s[(i + 1)..].strip
            return nil if left.empty? || right.empty?
            # Reject if there are further combinators in either half.
            return nil if has_combinator?(left) || has_combinator?(right)
            return [left, "child", right]
          elsif ch == " " || ch == "\t" || ch == "\n"
            left = s[0...i].strip
            rest = s[(i + 1)..].lstrip
            next i += 1 if rest.empty?
            # The next non-whitespace char must not be > / + / ~ — those
            # are picked up on their own iteration.
            if !left.empty? && !"<>+~,".include?(rest[0] || "")
              right = rest
              return nil if has_combinator?(left) || has_combinator?(right)
              return [left, "descendant", right]
            end
          end
        end
        i += 1
      end
      nil
    end

    def self.has_combinator?(s)
      depth = 0
      s.each_char do |ch|
        if ch == "["       then depth += 1
        elsif ch == "]"    then depth -= 1 if depth.positive?
        elsif depth.zero?
          return true if [" ", "\t", "\n", ">", "+", "~"].include?(ch)
        end
      end
      false
    end

    # Parse a simple CSS selector into the [tag, classes, id, attrs] form
    # that the C engine accepts. Returns nil if the selector uses
    # combinators or pseudo-classes (those force the Ruby fallback).
    #
    # Supported:
    #   tag                    div
    #   .class                 .product-card
    #   tag.class.other        span.price.big
    #   #id                    #main
    #   tag#id                 article#main
    #   [attr]                 [data-sku]
    #   [attr=val]             [data-sku="A1"]
    #   [attr*=val]            [class*=card]
    #   [attr^=val]            [href^=https]
    #   [attr$=val]            [href$=.pdf]
    #   [attr~=val]            [class~=primary]
    #   [attr|=val]            [lang|=en]
    #   ... and combinations
    def self.parse_selector(selector)
      return nil unless selector
      s = selector.to_s.strip
      return nil if s.empty?
      # Check for combinators / unsupported syntax outside [...] brackets,
      # since `*` and `~` are valid inside attribute operators.
      outside = s.gsub(/\[[^\]]*\]/, "")
      return nil if outside =~ /[\s>+~,*]/

      tag      = nil
      classes  = []
      id       = nil
      attrs    = []

      i = 0
      if (m = s[i..].match(/\A([a-zA-Z][\w-]*)/))
        tag = m[1].downcase
        i += m[0].length
      end

      while i < s.length
        case s[i]
        when "."
          m = s[i..].match(/\A\.([\w-]+)/)
          return nil unless m
          classes << m[1]
          i += m[0].length
        when "#"
          m = s[i..].match(/\A#([\w-]+)/)
          return nil unless m
          return nil if id # only one id allowed
          id = m[1]
          i += m[0].length
        when "["
          m = s[i..].match(/\A\[([\w:-]+)(?:([*^$~|]?=)["']?([^\]"']*)["']?)?\]/)
          return nil unless m
          attrs << [m[1], m[2], m[3]]
          i += m[0].length
        else
          return nil
        end
      end

      return nil if tag.nil? && classes.empty? && id.nil? && attrs.empty?
      return nil if classes.size > 8 || attrs.size > 8

      [tag, classes, id, attrs]
    end
  end
end
