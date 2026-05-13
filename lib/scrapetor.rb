# frozen_string_literal: true

require "scrapetor/version"
require "scrapetor/errors"
require "scrapetor/encoding"
require "scrapetor/cleaner"
require "scrapetor/money"
require "scrapetor/url"
require "scrapetor/fingerprint"
require "scrapetor/text_node"
require "scrapetor/selector"
require "scrapetor/sax"
require "scrapetor/dom"
require "scrapetor/dom/parser"
require "scrapetor/dom/selectors"
require "scrapetor/node"
require "scrapetor/node_set"
require "scrapetor/schema"
require "scrapetor/extractor"
require "scrapetor/document"
require "scrapetor/template_registry"
require "scrapetor/structured_data"
require "scrapetor/microdata"
require "scrapetor/page_type"
require "scrapetor/entities"
require "scrapetor/builder"
require "scrapetor/http"
require "scrapetor/native"
require "scrapetor/native_dom"
require "scrapetor/persistent_cache"
require "scrapetor/stream"
require "scrapetor/fetcher"
require "scrapetor/session"
require "scrapetor/robots"
require "scrapetor/sitemap"
require "scrapetor/pagination"
require "scrapetor/form"

module Scrapetor
  # ----- Parsing entry points -----

  def self.parse(html, base_url: nil, build_indexes: false)
    if PersistentCache.enabled? && html.is_a?(String) && !html.empty?
      cached = PersistentCache.load(html)
      if cached
        doc = Document.new(html, base_url: base_url,
                           build_indexes: build_indexes, native: cached)
        return doc
      end
    end
    doc = Document.new(html, base_url: base_url, build_indexes: build_indexes)
    if PersistentCache.enabled? && html.is_a?(String) && !html.empty?
      PersistentCache.store(html, doc.backing.native) rescue nil
    end
    doc
  end

  # `Scrapetor::HTML(html)` — capital-H convenience method.
  def self.HTML(html, base_url = nil)
    parse(html, base_url: base_url)
  end

  def self.parse_html(html, base_url: nil)
    parse(html, base_url: base_url)
  end

  def self.parse_fragment(html, base_url: nil)
    parse(html, base_url: base_url)
  end

  # Parse from an arbitrary IO-like (responds to `read`) or a file path.
  def self.parse_io(io, base_url: nil)
    parse(io.read, base_url: base_url)
  end

  def self.parse_file(path, base_url: nil)
    parse(File.read(path), base_url: base_url)
  end

  # Parse N documents in parallel via native pthread workers, releasing
  # the GVL for the duration. Returns Array<Scrapetor::Document> in the
  # same order as the input. Skips the in-memory parse cache (which is
  # GVL-bound); use single-document Scrapetor.parse for cache-friendly
  # workloads.
  #
  # Use this for batch jobs over distinct documents where parsing
  # dominates: pre-warming a fixture corpus, indexing a crawl, A/B
  # comparing parsed shapes. Falls through to a serial parse when only
  # one document is provided.
  def self.parallel_parse(htmls, threads: nil)
    htmls = Array(htmls)
    return [] if htmls.empty?
    return [parse(htmls.first)] if htmls.size == 1
    n = threads || default_parallel_threads(htmls.size)
    natives = Native::Document.parallel_parse(htmls, n)
    natives.each_with_index.map do |native, i|
      Document.new(htmls[i], native: native)
    end
  end

  def self.default_parallel_threads(n_items)
    cpu = begin
      require "etc"
      Etc.nprocessors
    rescue StandardError
      4
    end
    [n_items, cpu].min
  end

  # Run an extraction schema directly against a file or IO.
  def self.extract_file(path, schema, base_url: nil)
    extract(File.read(path), schema, base_url: base_url)
  end

  # `Scrapetor::HTML5(html)` — same parser, alternate name.
  def self.HTML5(*args, &block)
    parse(*args, &block)
  end

  # `Scrapetor::HTML.parse` / `.fragment` namespace.
  module HTML
    def self.parse(*args, &block)
      Scrapetor.parse(*args, &block)
    end

    def self.fragment(*args, &block)
      Scrapetor.parse_fragment(*args, &block)
    end
  end

  module HTML5
    def self.parse(*args, &block)
      Scrapetor.parse(*args, &block)
    end

    def self.fragment(*args, &block)
      Scrapetor.parse_fragment(*args, &block)
    end
  end

  # ----- Extraction DSL -----

  def self.schema(&block)
    Schema.build(&block)
  end

  def self.extract(html, schema = nil, base_url: nil, &block)
    parse(html, base_url: base_url).extract(schema, &block)
  end

  # Force the native streaming path. Raises if the schema can't compile.
  def self.extract_native(html, schema, base_url: nil)
    raise Error, "native extension not loaded" unless Native.available?
    desc = Native.compile_descriptor(schema)
    raise Error, "schema not native-compilable" unless desc
    Native.extract(html.to_s, desc, base_url)
  end

  # Force the Ruby reference path. Useful for parity tests + benchmarks.
  def self.extract_ruby(html, schema, base_url: nil)
    doc = parse(html, base_url: base_url)
    Extractor.run(doc, doc.backing, schema)
  end
end
