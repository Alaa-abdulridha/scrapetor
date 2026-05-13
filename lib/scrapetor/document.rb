# frozen_string_literal: true

module Scrapetor
  class Document
    attr_reader :base_url, :encoding

    def initialize(html, base_url: nil, build_indexes: false, encoding: :auto)
      @base_url = base_url
      raw = html.to_s
      if encoding == :auto
        @encoding = Scrapetor::Encoding.detect(raw)
        @html_str = Scrapetor::Encoding.to_utf8(raw)
      else
        @encoding = encoding.to_s
        @html_str = raw.dup.force_encoding(@encoding).encode("UTF-8", invalid: :replace, undef: :replace, replace: "")
      end
      @backing = nil # parsed lazily; native extract bypasses this entirely
      @selector_cache = {}
      @indexes_built = false
      @class_index = nil
      @id_index = nil
      @tag_index = nil
      # Hot-path slots (populated by backing()): keeping these
      # initialised silences "instance variable not initialized" and
      # makes the fast-path test a simple nil check.
      @native_doc     = nil
      @native_wrapper = nil
      @plan_cache     = nil
      @lazy_ids       = nil
      build_indexes! if build_indexes
    end

    def html_str
      @html_str
    end

    # CSS query entry point. Inlined hot path for the >95% case: a
    # selector with no `::` pseudo-element and a cache-hit native plan.
    # That bypasses backing.lazy_css, peel_pseudo_element, and the
    # method dispatch chain, dropping the per-call Ruby overhead to a
    # single Hash#[] + Struct.new + NodeSet.new.
    def css(selector)
      # Fast path: native backing, plain String selector, no pseudo-
      # element. One Hash lookup + one C call + two allocations.
      # @lazy_ids is the cached LazyIds class so the inner loop doesn't
      # pay a Module-path constant lookup per call.
      if @native_doc && selector.is_a?(String) && !selector.include?("::")
        plan = @plan_cache[selector]
        if plan
          return NodeSet.new(self, @lazy_ids.new(@native_wrapper, @native_doc, @native_doc.run_chain(plan, nil)))
        elsif !@plan_cache.key?(selector)
          plan = Scrapetor::Native.compile_selector_chain(selector)
          @plan_cache[selector] = plan || false
          if plan
            return NodeSet.new(self, @lazy_ids.new(@native_wrapper, @native_doc, @native_doc.run_chain(plan, nil)))
          end
        end
      end
      # Slow path: pseudo-element, comma, fallback, or non-native backing.
      bk = backing
      result = bk.respond_to?(:lazy_css) ? bk.lazy_css(selector) : bk.css(selector)
      if result.is_a?(Array) && (result.first.is_a?(String) || (result.empty? && pseudo_element?(selector)))
        return result
      end
      if @lazy_ids && result.is_a?(@lazy_ids)
        return NodeSet.new(self, result)
      end
      NodeSet.new(self, result.to_a)
    end

    # Run an array of CSS selectors in ONE Ruby/C boundary crossing.
    # On selector-heavy workloads (SerpApi-style pages with ~30
    # selectors per scrape) this amortises the per-query Ruby overhead
    # across all of them — N selectors cost roughly one selector
    # worth of Ruby dispatch, not N. Returns an Array of NodeSets (or
    # Arrays-of-strings, for `::text` / `::attr(name)` selectors)
    # parallel to the input.
    #
    #   title_ns, price_strs, hrefs = doc.batch_css(
    #     ["h1.title", ".price::text", "a::attr(href)"]
    #   )
    def batch_css(selectors)
      bk = backing
      unless bk.respond_to?(:batch_css)
        # Pure-Ruby Dom fallback — no native engine. Loop manually.
        return selectors.map { |s| css(s) }
      end
      bk.batch_css(self, selectors)
    end

    # Hash form: `{ name => selector, ... }` -> `{ name => result, ... }`.
    # The classic scrape pattern in two lines. Same one-boundary cost
    # as batch_css.
    def extract_css(map)
      keys = map.keys
      selectors = map.values
      results = batch_css(selectors)
      out = {}
      keys.each_with_index { |k, i| out[k] = results[i] }
      out
    end

    def at(selector)
      result = backing.at_css(selector)
      return nil if result.nil?
      return result if result.is_a?(String)
      Node.new(self, result)
    end
    alias at_css at
    alias search css

    def xpath(expr)
      result = backing.respond_to?(:xpath) ? backing.xpath(expr).to_a : []
      NodeSet.new(self, result)
    end

    def at_xpath(expr)
      xpath(expr).first
    end

    def traverse(&block)
      return enum_for(:traverse) unless block_given?
      backing.traverse { |n| yield(n.respond_to?(:element?) ? Node.new(self, n) : n) } if backing.respond_to?(:traverse)
      self
    end

    private

    def pseudo_element?(selector)
      selector.to_s =~ /::(text|attr\([^)]+\)|first-letter|first-line|before|after)\s*\z/i
    end

    public

    def root
      el = backing.at_css("html") || backing
      Node.new(self, el)
    end

    def text
      backing.text
    end
    alias content    text
    alias inner_text text

    def title
      n = backing.at_css("title")
      n && n.text
    end

    def body
      n = backing.at_css("body")
      n && Node.new(self, n)
    end

    def head
      n = backing.at_css("head")
      n && Node.new(self, n)
    end

    def html
      n = backing.at_css("html") || backing
      Node.new(self, n)
    end

    def to_html
      backing.to_html
    end
    alias to_s to_html

    # Nokogiri-compat predicates.
    def errors
      []
    end

    def html?
      true
    end

    def xml?
      false
    end

    # Structured-data extractors — for SEO/RAG/structured-content pipelines.

    def json_ld
      Scrapetor::StructuredData.json_ld(self)
    end

    def opengraph
      Scrapetor::StructuredData.opengraph(self)
    end

    def twitter_card
      Scrapetor::StructuredData.twitter_card(self)
    end

    def schema_org(type: nil)
      Scrapetor::StructuredData.schema_org(self, type: type)
    end

    def microdata
      Scrapetor::Microdata.extract(self)
    end

    def rdfa
      Scrapetor::RDFa.extract(self)
    end

    def page_type
      Scrapetor::PageType.detect(self)
    end

    def extract(schema = nil, &block)
      schema ||= Schema.build(&block)
      if Native.available?
        result = extract_via_native(schema)
        return result unless result.nil?
      end
      Extractor.run(self, backing, schema)
    end

    private

    # Try the native path. Returns the result Hash on success, nil if the
    # schema can't compile (caller falls back to Ruby).
    #
    # Schemas with both top-level fields AND repeated groups run two
    # native passes — the engine supports one active record at a time, so
    # a synthetic <html>-bound root for top-level fields can't co-exist
    # with a repeated group inside the same scan. Two-pass cost is a
    # second 65μs scan; still ~10× ahead of Nokolexbor at this size.
    def extract_via_native(schema)
      has_fields = schema.fields.any?
      has_groups = schema.groups.any?
      return nil unless has_fields || has_groups

      # Common case: only repeated groups — one pass, no schema split.
      if has_groups && !has_fields
        desc = Native.compile_descriptor(schema)
        return nil unless desc
        return Native.extract(@html_str, desc, @base_url)
      end

      # Top-level fields only — one pass via synthetic root.
      if has_fields && !has_groups
        desc = Native.compile_descriptor(schema)
        return nil unless desc
        raw = Native.extract(@html_str, desc, @base_url)
        root_records = raw[Native::SYNTHETIC_ROOT]
        return nil if !root_records.is_a?(Array) || root_records.empty?
        return root_records[0]
      end

      # Mixed: two-pass. The C engine handles one active record at a
      # time, so a synthetic root for top-level fields can't run in
      # the same scan as a repeated group. We split the schema into
      # two sub-descriptors and run extract twice. Both sub-descriptors
      # are memoised on the original schema so the split itself only
      # allocates on the first call.
      groups_desc = Native.split_descriptor(schema, :groups)
      return nil unless groups_desc
      result = Native.extract(@html_str, groups_desc, @base_url)

      fields_desc = Native.split_descriptor(schema, :fields)
      return nil unless fields_desc
      raw = Native.extract(@html_str, fields_desc, @base_url)
      root_records = raw[Native::SYNTHETIC_ROOT]
      return nil if !root_records.is_a?(Array) || root_records.empty?

      root_records[0].merge(result)
    end

    public

    def stats
      {
        classes: @class_index ? @class_index.size : 0,
        ids: @id_index ? @id_index.size : 0,
        tags: @tag_index ? @tag_index.size : 0,
        selector_cache_size: @selector_cache.size,
        indexes_built: @indexes_built
      }
    end

    def backing
      return @backing if @backing
      @backing =
        if defined?(Scrapetor::Native::DocumentWrapper) && Scrapetor::Native::AVAILABLE_DOM
          Scrapetor::Native::DocumentWrapper.new(
            Scrapetor::Native::Document.parse(@html_str)
          )
        else
          Dom::Parser.parse(@html_str)
        end
      # Cache the hot-path slots so Document#css can skip the indirection.
      if defined?(Scrapetor::Native::DocumentWrapper) &&
         @backing.is_a?(Scrapetor::Native::DocumentWrapper)
        @native_doc     = @backing.native
        @native_wrapper = @backing
        @plan_cache     = @backing.instance_variable_get(:@compile_cache)
        @lazy_ids       = Scrapetor::Native::DocumentWrapper::LazyIds
      end
      @backing
    end

    # Phase-2 hooks: structural indexes. Built on demand. The native
    # backend will replace these with arena-resident indexes.
    def class_index
      build_indexes! unless @indexes_built
      @class_index
    end

    def id_index
      build_indexes! unless @indexes_built
      @id_index
    end

    def tag_index
      build_indexes! unless @indexes_built
      @tag_index
    end

    def all_elements
      build_indexes! unless @indexes_built
      @all_elements
    end

    def run_selector(selector, scope)
      plan = @selector_cache[selector] ||= Selector.compile(selector)
      Selector.execute(self, plan, scope)
    end

    def cache_selector(selector)
      @selector_cache[selector] ||= Selector.compile(selector)
    end

    def selector_cache_size
      @selector_cache.size
    end

    private

    def build_indexes!
      return if @indexes_built
      @class_index = Hash.new { |h, k| h[k] = [] }
      @id_index = {}
      @tag_index = Hash.new { |h, k| h[k] = [] }
      @all_elements = backing.css("*").to_a
      @all_elements.each do |el|
        @tag_index[el.name.to_sym] << el
        id = el["id"]
        @id_index[id] ||= el if id
        cls = el["class"]
        if cls
          cls.split(/\s+/).each { |c| @class_index[c] << el unless c.empty? }
        end
      end
      @indexes_built = true
    end
  end
end
