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

module Scrapetor
  # ----- Parsing entry points -----

  def self.parse(html, base_url: nil, build_indexes: false)
    Document.new(html, base_url: base_url, build_indexes: build_indexes)
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
