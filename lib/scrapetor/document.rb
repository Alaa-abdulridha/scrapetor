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
      build_indexes! if build_indexes
    end

    def html_str
      @html_str
    end

    def css(selector)
      NodeSet.new(self, backing.css(selector).to_a)
    end

    def at(selector)
      n = backing.at_css(selector)
      n && Node.new(self, n)
    end
    alias at_css at
    alias search css

    def xpath(expr)
      NodeSet.new(self, backing.xpath(expr).to_a)
    end

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
      @backing ||=
        if defined?(Scrapetor::Native::DocumentWrapper) && Scrapetor::Native::AVAILABLE_DOM
          Scrapetor::Native::DocumentWrapper.new(
            Scrapetor::Native::Document.parse(@html_str)
          )
        else
          Dom::Parser.parse(@html_str)
        end
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
