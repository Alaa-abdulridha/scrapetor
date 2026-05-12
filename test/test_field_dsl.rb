# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

class TestFieldDSL < Minitest::Test
  HTML = <<~HTML
    <html><body>
      <div class="card" data-active="true" data-stock="42">
        <h2 class="t">Widget</h2>
        <p class="desc"><strong>Premium</strong> widget &amp; gadget.</p>
        <ul class="tags">
          <li class="tag">red</li>
          <li class="tag">blue</li>
          <li class="tag">heavy</li>
        </ul>
        <span class="csv-tags">red, blue, heavy</span>
        <span class="opts">a;b;c</span>
        <span class="num"> 42 reviews</span>
        <span class="missing-target"></span>
        <span class="payload">{"id":1,"name":"X"}</span>
      </div>
    </body></html>
  HTML

  def doc
    @doc ||= Scrapetor.parse(HTML, base_url: "https://example.com/")
  end

  # ----- :html field type -----

  def test_html_field_captures_inner_html
    result = doc.extract do
      field :desc, from: ".desc", type: :html
    end
    assert_includes result[:desc], "<strong>Premium</strong>"
  end

  # ----- :list field type -----

  def test_list_with_default_delimiter
    result = doc.extract do
      field :tags, from: ".csv-tags", type: :list
    end
    assert_equal %w[red blue heavy], result[:tags]
  end

  def test_list_with_custom_delimiter
    result = doc.extract do
      field :opts, from: ".opts", type: :list, delimiter: ";"
    end
    assert_equal %w[a b c], result[:opts]
  end

  # ----- :json field type -----

  def test_json_field_parses
    result = doc.extract do
      field :payload, from: ".payload", type: :json
    end
    assert_equal({ "id" => 1, "name" => "X" }, result[:payload])
  end

  def test_json_field_handles_bad_json
    bad_html = '<div class="x">{not valid}</div>'
    result = Scrapetor.parse(bad_html).extract do
      field :x, from: ".x", type: :json
    end
    assert_nil result[:x]
  end

  # ----- :boolean field type -----

  def test_boolean_from_attribute
    result = doc.extract do
      field :active, from: ".card", attr: "data-active", type: :boolean
    end
    assert_equal true, result[:active]
  end

  def test_boolean_falsy_string
    html = '<div class="x">false</div>'
    result = Scrapetor.parse(html).extract do
      field :x, from: ".x", type: :boolean
    end
    assert_equal false, result[:x]
  end

  # ----- :array (= multi: true) -----

  def test_array_alias_for_multi
    result = doc.extract do
      field :tags, from: ".tag", type: :array
    end
    assert_equal %w[red blue heavy], result[:tags]
  end

  # ----- default + required -----

  def test_default_used_when_missing
    result = doc.extract do
      field :title, from: ".does-not-exist", default: "Untitled"
    end
    assert_equal "Untitled", result[:title]
  end

  def test_default_not_used_when_present
    result = doc.extract do
      field :title, from: ".t", default: "Untitled"
    end
    assert_equal "Widget", result[:title]
  end

  def test_required_raises_when_missing
    assert_raises(Scrapetor::ExtractionError) do
      doc.extract do
        field :title, from: ".does-not-exist", required: true
      end
    end
  end

  def test_required_passes_when_present
    result = doc.extract do
      field :title, from: ".t", required: true
    end
    assert_equal "Widget", result[:title]
  end

  # ----- transform -----

  def test_transform_post_processes
    result = doc.extract do
      field :title, from: ".t", transform: ->(s) { s.upcase }
    end
    assert_equal "WIDGET", result[:title]
  end

  def test_transform_with_type_coercion
    result = doc.extract do
      field :stock, from: ".card", attr: "data-stock", type: :integer,
                    transform: ->(n) { n * 2 }
    end
    assert_equal 84, result[:stock]
  end

  def test_transform_skipped_when_default_applies_to_nil
    result = doc.extract do
      field :title, from: ".does-not-exist", default: "X",
                    transform: ->(s) { s.upcase }
    end
    # default applies, then transform runs on it
    assert_equal "X", result[:title]
  end

  # ----- from: [array] fallback selectors -----

  def test_from_array_picks_first_match
    result = doc.extract do
      field :title, from: [".does-not-exist", ".t", ".desc"]
    end
    assert_equal "Widget", result[:title]
  end

  def test_from_array_returns_nil_if_no_match
    result = doc.extract do
      field :title, from: [".a", ".b", ".c"]
    end
    assert_nil result[:title]
  end

  def test_from_array_with_default
    result = doc.extract do
      field :title, from: [".a", ".b"], default: "Fallback"
    end
    assert_equal "Fallback", result[:title]
  end

  # ----- inside repeated groups -----

  def test_new_options_work_in_repeated_group
    html = <<~HTML
      <html><body>
        <div class="card"><h2>A</h2><span class="p">1</span></div>
        <div class="card"><h2>B</h2></div>
      </body></html>
    HTML
    result = Scrapetor.parse(html).extract do
      repeated ".card", as: :cards do
        field :title, from: "h2"
        field :pages, from: ".p", type: :integer, default: 0
      end
    end
    assert_equal 1, result[:cards][0][:pages]
    assert_equal 0, result[:cards][1][:pages]
  end
end
