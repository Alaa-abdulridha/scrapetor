# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "tempfile"
require "scrapetor"

class TestExtras < Minitest::Test
  # ----- Entities -----

  def test_entities_decodes_named
    assert_equal "©", Scrapetor::Entities.decode("&copy;")
    assert_equal "® ™ — …", Scrapetor::Entities.decode("&reg; &trade; &mdash; &hellip;")
  end

  def test_entities_decodes_numeric
    assert_equal "A", Scrapetor::Entities.decode("&#65;")
    assert_equal "A", Scrapetor::Entities.decode("&#x41;")
    assert_equal "—", Scrapetor::Entities.decode("&#8212;")
  end

  def test_entities_passes_unknown
    assert_equal "&unknown;", Scrapetor::Entities.decode("&unknown;")
    assert_equal "no entity", Scrapetor::Entities.decode("no entity")
  end

  def test_entities_mixed_text
    assert_equal '"hello" & goodbye',
                 Scrapetor::Entities.decode("&quot;hello&quot; &amp; goodbye")
  end

  # ----- Plan cache -----

  def test_schema_dump_and_load_roundtrip
    schema = Scrapetor.schema do
      field :title, from: ".t", clean: true
      repeated ".card", as: :cards do
        field :name, from: "h2"
        field :price, from: ".p", type: :money
      end
    end
    blob = schema.dump
    assert_kind_of String, blob
    restored = Scrapetor::Schema.load(blob)
    assert_equal schema.fields.size, restored.fields.size
    assert_equal schema.groups.first.fields.size, restored.groups.first.fields.size
  end

  def test_schema_dump_to_file_and_load_file
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :t, from: ".t"
      end
    end
    Tempfile.open("scrapetor_plan") do |f|
      Scrapetor::Schema.dump_to_file(schema, f.path)
      restored = Scrapetor::Schema.load_file(f.path)
      assert_equal :xs, restored.groups.first.name
    end
  end

  def test_schema_dump_rejects_transform
    schema = Scrapetor.schema do
      field :title, from: ".t", transform: ->(s) { s.upcase }
    end
    assert_raises(Scrapetor::SchemaError) { schema.dump }
  end

  def test_loaded_schema_extracts_correctly
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :t, from: ".t"
        field :p, from: ".p", type: :money
      end
    end
    blob = schema.dump
    restored = Scrapetor::Schema.load(blob)

    html = '<div class="x"><span class="t">A</span><span class="p">$1.99</span></div>'
    result = Scrapetor.parse(html).extract(restored)
    assert_equal "A", result[:xs][0][:t]
    assert_in_delta 1.99, result[:xs][0][:p], 0.001
  end

  # ----- Selector path helpers -----

  def test_node_css_path_with_id
    html = '<html><body><div id="main"><p class="x">y</p></div></body></html>'
    doc = Scrapetor.parse(html)
    n = doc.at(".x")
    path = n.css_path
    assert_includes path, "#main"
    assert_includes path, "p:nth-of-type(1)"
  end

  def test_node_css_path_without_id
    html = '<html><body><ul><li>a</li><li>b</li><li>c</li></ul></body></html>'
    doc = Scrapetor.parse(html)
    second_li = doc.css("li")[1]
    assert_includes second_li.css_path, "li:nth-of-type(2)"
  end

  def test_node_xpath_path
    html = '<html><body><div><p>x</p></div></body></html>'
    p = Scrapetor.parse(html).at("p")
    assert_includes p.xpath_path.downcase, "p"
  end

  # ----- File / IO entry points -----

  def test_parse_file
    Tempfile.open(%w[scrapetor .html]) do |f|
      f.write('<html><body><h1 class="t">Hi</h1></body></html>')
      f.flush
      doc = Scrapetor.parse_file(f.path)
      assert_equal "Hi", doc.at(".t").text
    end
  end

  def test_parse_io
    io = StringIO.new("<html><body><p>x</p></body></html>")
    doc = Scrapetor.parse_io(io)
    assert_equal "x", doc.at("p").text
  end

  def test_extract_file
    schema = Scrapetor.schema do
      field :t, from: ".t"
    end
    Tempfile.open(%w[scrapetor .html]) do |f|
      f.write('<html><body><span class="t">hi</span></body></html>')
      f.flush
      result = Scrapetor.extract_file(f.path, schema)
      assert_equal "hi", result[:t]
    end
  end
end
