# frozen_string_literal: true

require "json"

module Scrapetor
  # Schema execution. The hot path operates on raw Nokolexbor nodes and
  # inlines coercion — no Scrapetor::Node allocations per emitted field.
  module Extractor
    def self.run(doc, scope, schema)
      result = {}
      base_url = doc.respond_to?(:base_url) ? doc.base_url : nil
      schema.fields.each do |f|
        result[f.name] = extract_field(scope, f, base_url)
      end
      schema.groups.each do |g|
        result[g.name] = run_group(scope, g, base_url)
      end
      result
    end

    def self.run_group(scope, group, base_url)
      out = []
      scope.css(group.selector).each do |sub|
        inner = {}
        group.fields.each { |f| inner[f.name] = extract_field(sub, f, base_url) }
        group.groups.each { |gg| inner[gg.name] = run_group(sub, gg, base_url) }
        out << inner
      end
      out
    end

    def self.extract_field(scope, f, base_url)
      selectors = f.selector.is_a?(Array) ? f.selector : [f.selector]
      value =
        if f.multi
          extract_multi(scope, selectors, f, base_url)
        else
          extract_single(scope, selectors, f, base_url)
        end

      # default + required
      missing = value.nil? || (f.multi && value.empty?)
      value = f.default if missing && !f.default.nil?

      if f.required && (value.nil? || (f.multi && value.respond_to?(:empty?) && value.empty?))
        raise ExtractionError, "required field `#{f.name}` not found"
      end

      # transform last (after coerce + default)
      value = f.transform.call(value) if f.transform && !value.nil?
      value
    end

    def self.extract_single(scope, selectors, f, base_url)
      selectors.each do |sel|
        n = sel ? scope.at_css(sel) : scope
        next if n.nil?
        raw = extract_raw(n, f)
        next if raw.nil?
        return coerce(raw, f, base_url, n)
      end
      nil
    end

    def self.extract_multi(scope, selectors, f, base_url)
      out = []
      selectors.each do |sel|
        nodes = sel ? scope.css(sel) : [scope]
        nodes.each do |n|
          raw = extract_raw(n, f)
          next if raw.nil?
          v = coerce(raw, f, base_url, n)
          out << v unless v.nil?
        end
        break unless out.empty?
      end
      out
    end

    def self.extract_raw(node, f)
      return node.inner_html if f.type == :html && f.attr_str.nil?
      if f.attr_str
        node[f.attr_str]
      else
        node.text
      end
    end

    def self.coerce(raw, f, base_url, _node)
      return nil if raw.nil?
      v = raw
      v = Cleaner.clean(v) if f.clean
      case f.type
      when :text     then v
      when :integer  then int_coerce(v)
      when :float    then float_coerce(v)
      when :money    then Money.parse(v)
      when :url      then f.normalize_url ? URL.absolute(v, base_url) : v
      when :date     then date_coerce(v)
      when :json     then json_coerce(v)
      when :boolean  then bool_coerce(v)
      when :list     then list_coerce(v, f.delimiter)
      when :html     then v # already inner_html
      else v
      end
    end

    def self.int_coerce(v)
      s = v.to_s.gsub(/[^\d\-]/, "")
      s.empty? || s == "-" ? nil : s.to_i
    end

    def self.float_coerce(v)
      s = v.to_s.gsub(/[^\d.\-]/, "")
      s.empty? || s == "-" || s == "." ? nil : s.to_f
    end

    def self.date_coerce(v)
      require "date"
      ::Date.parse(v.to_s)
    rescue ::ArgumentError, ::TypeError
      nil
    end

    def self.json_coerce(v)
      JSON.parse(v.to_s)
    rescue JSON::ParserError
      nil
    end

    TRUTHY_STRINGS = %w[true yes 1 on enabled].freeze
    FALSY_STRINGS  = %w[false no 0 off disabled].freeze
    private_constant :TRUTHY_STRINGS, :FALSY_STRINGS

    def self.bool_coerce(v)
      s = v.to_s.strip.downcase
      return true  if TRUTHY_STRINGS.include?(s)
      return false if FALSY_STRINGS.include?(s)
      return true  if s == "yes"
      nil
    end

    def self.list_coerce(v, delimiter)
      v.to_s.split(delimiter).map(&:strip).reject(&:empty?)
    end
  end
end
