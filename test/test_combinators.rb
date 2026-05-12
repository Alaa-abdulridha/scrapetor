# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

# Native + Ruby combinator support: descendant (A B) and child (A > B).
class TestCombinators < Minitest::Test
  HTML = <<~HTML
    <html><body>
      <div class="product">
        <header>
          <h2 class="title">Product Title</h2>
        </header>
        <div class="meta">
          <span class="price">$19.99</span>
          <span class="title">Subtitle in meta</span>
        </div>
      </div>
      <div class="other">
        <h2 class="title">Other Title</h2>
      </div>
    </body></html>
  HTML

  def doc
    @doc ||= Scrapetor.parse(HTML)
  end

  # ----- Descendant combinator -----

  def test_descendant_combinator_native_path
    schema = Scrapetor.schema do
      field :t, from: ".product .title"
    end
    desc = Scrapetor::Native.compile_descriptor(schema)
    assert desc, "descendant combinator should compile to native"
    result = doc.extract(schema)
    # First .title descendant of .product (h2 inside header)
    assert_equal "Product Title", result[:t]
  end

  def test_descendant_skips_outside_context
    schema = Scrapetor.schema do
      field :t, from: ".product .price"
    end
    result = doc.extract(schema)
    assert_equal "$19.99", result[:t]
  end

  def test_descendant_inside_group
    html = <<~HTML
      <html><body>
        <div class="card">
          <div class="info"><span class="t">A</span></div>
          <span class="t">no-info</span>
        </div>
        <div class="card">
          <div class="info"><span class="t">B</span></div>
        </div>
      </body></html>
    HTML
    result = Scrapetor.parse(html).extract do
      repeated ".card", as: :cards do
        field :inner, from: ".info .t"
      end
    end
    assert_equal "A", result[:cards][0][:inner]
    assert_equal "B", result[:cards][1][:inner]
  end

  # ----- Child combinator -----

  def test_child_combinator_native_path
    schema = Scrapetor.schema do
      field :direct, from: ".product > header"
    end
    desc = Scrapetor::Native.compile_descriptor(schema)
    assert desc, "child combinator should compile to native"
    result = doc.extract(schema)
    # The h2 inside header — text combined
    assert_includes result[:direct], "Product Title"
  end

  def test_child_does_not_match_non_direct
    html = <<~HTML
      <html><body>
        <div class="a">
          <div class="middle">
            <span class="b">deep</span>
          </div>
          <span class="b">direct</span>
        </div>
      </body></html>
    HTML
    result = Scrapetor.parse(html).extract do
      field :direct, from: ".a > .b"
    end
    assert_equal "direct", result[:direct]
  end

  def test_child_combinator_inside_group
    html = <<~HTML
      <html><body>
        <ul class="lst">
          <li class="item"><span class="t">A</span></li>
          <li class="item"><div><span class="t">B-deep</span></div></li>
        </ul>
      </body></html>
    HTML
    result = Scrapetor.parse(html).extract do
      repeated ".item", as: :items do
        field :direct, from: ".item > .t"
      end
    end
    assert_equal "A", result[:items][0][:direct]
    assert_nil result[:items][1][:direct]
  end

  # ----- Native compiles fall through gracefully -----

  def test_multi_combinator_falls_back_to_ruby
    schema = Scrapetor.schema do
      field :t, from: ".a .b .c"
    end
    refute Scrapetor::Native.compile_descriptor(schema),
           "multi-combinator selector must fall back to Ruby"
  end

  def test_sibling_combinator_falls_back
    schema = Scrapetor.schema do
      field :t, from: ".a + .b"
    end
    refute Scrapetor::Native.compile_descriptor(schema)
  end

  # ----- Parity: native vs Ruby -----

  def test_native_matches_ruby_for_descendant
    html = HTML
    schema = Scrapetor.schema do
      field :t,     from: ".product .title"
      field :price, from: ".product .price"
    end
    ruby   = Scrapetor.extract_ruby(html, schema)
    native = Scrapetor.extract_native(html, schema)
    # SYNTHETIC_ROOT wraps the native output for top-level fields.
    nr = native[Scrapetor::Native::SYNTHETIC_ROOT][0]
    assert_equal ruby[:t],     nr[:t]
    assert_equal ruby[:price], nr[:price]
  end
end
