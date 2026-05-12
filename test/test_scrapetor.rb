# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

class TestScrapetor < Minitest::Test
  HTML = <<~HTML
    <!DOCTYPE html>
    <html>
      <body>
        <main id="main">
          <div class="product-card" data-sku="A1">
            <h2 class="title">Widget A</h2>
            <span class="price">$19.99</span>
            <a href="/widget-a">link</a>
            <img src="/img/a.png" />
          </div>
          <div class="product-card" data-sku="B2">
            <h2 class="title">Widget B</h2>
            <span class="price">$24.50</span>
            <a href="/widget-b">link</a>
            <img src="/img/b.png" />
          </div>
          <div class="product-card featured" data-sku="C3">
            <h2 class="title">Widget C</h2>
            <span class="price">USD 1,299.00</span>
            <a href="https://other.test/widget-c">link</a>
            <img src="/img/c.png" />
          </div>
        </main>
      </body>
    </html>
  HTML

  def setup
    @doc = Scrapetor.parse(HTML, base_url: "https://example.com/")
  end

  def test_parse_returns_document
    assert_kind_of Scrapetor::Document, @doc
  end

  def test_css_class_lookup
    assert_equal 3, @doc.css(".product-card").size
  end

  def test_at_returns_first_match
    n = @doc.at(".title")
    assert_equal "Widget A", n.text
  end

  def test_css_id_lookup
    assert_equal 1, @doc.css("#main").size
  end

  def test_descendant_combinator
    titles = @doc.css(".product-card .title")
    assert_equal 3, titles.size
    assert_equal "Widget A", titles.first.text
  end

  def test_child_combinator
    direct = @doc.css(".product-card > .title")
    assert_equal 3, direct.size
  end

  def test_tag_class_intersection
    spans = @doc.css("span.price")
    assert_equal 3, spans.size
  end

  def test_attribute_presence
    cards = @doc.css("[data-sku]")
    assert_equal 3, cards.size
  end

  def test_attribute_equality
    a = @doc.css('[data-sku="A1"]')
    assert_equal 1, a.size
  end

  def test_node_attr_helpers
    a = @doc.at("a")
    assert_equal "/widget-a", a[:href]
    assert_equal "/widget-a", a.attr("href")
  end

  def test_money_parsing
    cards = @doc.css(".product-card")
    assert_in_delta 19.99, cards[0].at(".price").money, 0.001
    assert_in_delta 24.50, cards[1].at(".price").money, 0.001
    assert_in_delta 1299.00, cards[2].at(".price").money, 0.001
  end

  def test_absolute_url
    a = @doc.at("a")
    assert_equal "https://example.com/widget-a", a.absolute_url
  end

  def test_absolute_url_preserves_absolute
    last = @doc.css("a").to_a.last
    assert_equal "https://other.test/widget-c", last.absolute_url
  end

  def test_schema_extraction
    schema = Scrapetor.schema do
      repeated ".product-card", as: :products do
        field :title, from: ".title", clean: true
        field :price, from: ".price", type: :money
        field :url, from: "a", attr: :href, type: :url, normalize_url: true
        field :image, from: "img", attr: :src, type: :url, normalize_url: true
      end
    end
    result = @doc.extract(schema)
    assert_equal 3, result[:products].size
    assert_equal "Widget A", result[:products][0][:title]
    assert_in_delta 19.99, result[:products][0][:price], 0.001
    assert_equal "https://example.com/widget-a", result[:products][0][:url]
    assert_equal "https://example.com/img/a.png", result[:products][0][:image]
    assert_equal "https://other.test/widget-c", result[:products][2][:url]
  end

  def test_inline_extract_block
    result = @doc.extract do
      repeated ".product-card", as: :products do
        field :title, from: ".title", clean: true
      end
    end
    assert_equal 3, result[:products].size
  end

  def test_selector_cache_when_explicitly_used
    @doc.cache_selector(".product-card")
    @doc.cache_selector(".product-card")
    @doc.cache_selector(".title")
    assert_equal 2, @doc.stats[:selector_cache_size]
  end

  def test_stats
    @doc.class_index # trigger lazy index build
    s = @doc.stats
    assert_operator s[:classes], :>=, 3
    assert_operator s[:tags], :>=, 1
  end

  def test_fingerprint_stable
    cards = @doc.css(".product-card")
    fp1 = cards[0].fingerprint
    fp2 = cards[1].fingerprint
    assert_kind_of Integer, fp1
    assert_equal fp1, fp2, "Structurally identical cards should fingerprint identically"
  end

  def test_money_handles_european_format
    assert_in_delta 1234.56, Scrapetor::Money.parse("EUR 1.234,56"), 0.001
    assert_in_delta 1234.56, Scrapetor::Money.parse("$1,234.56"), 0.001
    assert_in_delta 19.99, Scrapetor::Money.parse("$19.99"), 0.001
    assert_nil Scrapetor::Money.parse("no digits here")
  end

  def test_url_absolute
    assert_equal "https://example.com/x", Scrapetor::URL.absolute("/x", "https://example.com/")
    assert_equal "https://other.test/x", Scrapetor::URL.absolute("https://other.test/x", "https://example.com/")
    assert_nil Scrapetor::URL.absolute(nil)
  end

  def test_cleaner
    assert_equal "hello world", Scrapetor::Cleaner.clean("   hello\n  world  ")
    assert_nil Scrapetor::Cleaner.clean(nil)
  end

  def test_node_set_enumerable
    titles = @doc.css(".title").map(&:text)
    assert_equal ["Widget A", "Widget B", "Widget C"], titles
  end

  def test_native_extension_loaded
    skip "native extension not built" unless Scrapetor::Native.available?
    assert Scrapetor::Native.available?
  end

  def test_native_and_ruby_parity
    skip "native extension not built" unless Scrapetor::Native.available?
    schema = Scrapetor.schema do
      repeated ".product-card", as: :products do
        field :title, from: ".title", clean: true
        field :price, from: ".price", type: :money
        field :url, from: "a", attr: :href, type: :url, normalize_url: true
      end
    end
    native = Scrapetor.extract_native(HTML, schema, base_url: "https://example.com/")
    ruby   = Scrapetor.extract_ruby(HTML, schema, base_url: "https://example.com/")
    assert_equal ruby, native
  end

  def test_native_handles_entities
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="card"><span class="t">A &amp; B &lt;c&gt; &#39;d&#39; &#x2014;</span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :items do
        field :t, from: ".t"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "A & B <c> 'd' —", result[:items][0][:t]
  end

  def test_native_id_selector
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="card"><h2 id="hdr">Hello</h2></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :h, from: "#hdr"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "Hello", result[:xs][0][:h]
  end

  def test_native_attribute_presence
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="card"><a href="/a" data-track="yes">A</a><a href="/b">B</a></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: "[data-track]"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "A", result[:xs][0][:t]
  end

  def test_native_attribute_equality
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="card"><a data-sku="A1">A</a><a data-sku="B2">B</a></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :pick, from: '[data-sku="B2"]'
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "B", result[:xs][0][:pick]
  end

  def test_native_attribute_substring_ops
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="x"><a href="https://example.com/a.pdf" class="dl">A</a><a href="/b.png">B</a></div>'
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :pdf,  from: '[href$=".pdf"]'
        field :http, from: '[href^="http"]'
        field :ex,   from: '[href*="example"]'
      end
    end
    r = Scrapetor.extract_native(html, schema)
    assert_equal "A", r[:xs][0][:pdf]
    assert_equal "A", r[:xs][0][:http]
    assert_equal "A", r[:xs][0][:ex]
  end

  def test_native_multi_class_selector
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="card"><span class="badge new">N</span><span class="badge">B</span><span class="new">X</span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :pick, from: ".badge.new"
      end
    end
    r = Scrapetor.extract_native(html, schema)
    assert_equal "N", r[:xs][0][:pick]
  end

  def test_native_tag_id_combination
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="x"><span id="x">no</span><article id="x">yes</article></div>'
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :pick, from: "article#x"
      end
    end
    r = Scrapetor.extract_native(html, schema)
    assert_equal "yes", r[:xs][0][:pick]
  end

  def test_native_multi_field_collects_all
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="x"><span class="b">1</span><span class="b">2</span><span class="b">3</span></div>'
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :bs, from: ".b", multi: true
      end
    end
    r = Scrapetor.extract_native(html, schema)
    assert_equal %w[1 2 3], r[:xs][0][:bs]
  end

  def test_native_multi_attr_collects_all
    skip "native extension not built" unless Scrapetor::Native.available?
    html = '<div class="x"><a href="/a">A</a><a href="/b">B</a><a href="/c">C</a></div>'
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :urls, from: "a", attr: :href, type: :url, normalize_url: true, multi: true
      end
    end
    r = Scrapetor.extract_native(html, schema, base_url: "https://example.com/")
    assert_equal %w[https://example.com/a https://example.com/b https://example.com/c], r[:xs][0][:urls]
  end

  def test_native_skips_script_and_style
    skip "native extension not built" unless Scrapetor::Native.available?
    html = <<~HTML
      <div class="x">
        <span class="t">keep</span>
        <script>var z = "<span class='t'>noise</span>";</script>
        <style>.t { color: <span class="t">red</span>; }</style>
      </div>
    HTML
    schema = Scrapetor.schema do
      repeated ".x", as: :xs do
        field :t, from: ".t", clean: true
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal 1, result[:xs].size
    assert_equal "keep", result[:xs][0][:t]
  end

  # ----- Top-level fields (single-record extraction) -----

  def test_top_level_fields_extracted
    html = <<~HTML
      <html><body>
        <h1>Hello</h1>
        <span class="price">$19.99</span>
        <a href="/x" class="lk">link</a>
      </body></html>
    HTML
    doc = Scrapetor.parse(html, base_url: "https://example.com/")
    result = doc.extract do
      field :title, from: "h1", clean: true
      field :price, from: ".price", type: :money
      field :url,   from: ".lk", attr: :href, type: :url, normalize_url: true
    end
    assert_equal "Hello", result[:title]
    assert_in_delta 19.99, result[:price], 0.001
    assert_equal "https://example.com/x", result[:url]
  end

  def test_top_level_and_repeated_fields_combined
    html = <<~HTML
      <html><body>
        <h1>Big Sale</h1>
        <ul>
          <li class="item">A</li>
          <li class="item">B</li>
        </ul>
      </body></html>
    HTML
    result = Scrapetor.parse(html).extract do
      field :title, from: "h1"
      repeated ".item", as: :items do
        field :name, from: ".item"
      end
    end
    assert_equal "Big Sale", result[:title]
    assert_equal %w[A B], result[:items].map { |i| i[:name] }
  end

  # ----- Structured data: JSON-LD / OpenGraph / Schema.org -----

  def test_json_ld_extraction
    html = <<~HTML
      <html>
        <head>
          <script type="application/ld+json">{"@context":"https://schema.org","@type":"Product","name":"Widget","offers":{"price":"19.99"}}</script>
          <script type="application/ld+json">[{"@type":"Organization","name":"Acme"}]</script>
        </head>
        <body><p>x</p></body>
      </html>
    HTML
    doc = Scrapetor.parse(html)
    data = doc.json_ld
    assert_equal 2, data.size
    assert_equal "Widget", data[0]["name"]
    assert_equal "Acme",   data[1]["name"]
  end

  def test_json_ld_graph_unwrap
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">{"@graph":[{"@type":"Product","name":"A"},{"@type":"Product","name":"B"}]}</script>
      </head><body>x</body></html>
    HTML
    doc = Scrapetor.parse(html)
    data = doc.json_ld
    assert_equal 2, data.size
    assert_equal %w[A B], data.map { |x| x["name"] }
  end

  def test_opengraph_extraction
    html = <<~HTML
      <html><head>
        <meta property="og:title" content="Page Title">
        <meta property="og:image" content="https://example.com/img.png">
        <meta property="og:type"  content="article">
      </head><body>x</body></html>
    HTML
    doc = Scrapetor.parse(html)
    og = doc.opengraph
    assert_equal "Page Title", og["title"]
    assert_equal "article", og["type"]
  end

  def test_twitter_card_extraction
    html = <<~HTML
      <html><head>
        <meta name="twitter:card"  content="summary_large_image">
        <meta name="twitter:title" content="T">
      </head><body>x</body></html>
    HTML
    doc = Scrapetor.parse(html)
    tw = doc.twitter_card
    assert_equal "summary_large_image", tw["card"]
    assert_equal "T", tw["title"]
  end

  def test_schema_org_filter_by_type
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">[
          {"@type":"Product","name":"A"},
          {"@type":"Article","headline":"X"}
        ]</script>
      </head><body>x</body></html>
    HTML
    doc = Scrapetor.parse(html)
    products = doc.schema_org(type: "Product")
    assert_equal 1, products.size
    assert_equal "A", products[0]["name"]
  end

  def test_json_ld_ignores_invalid
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">{not valid json}</script>
        <script type="application/ld+json">{"@type":"Product","name":"ok"}</script>
      </head><body>x</body></html>
    HTML
    data = Scrapetor.parse(html).json_ld
    assert_equal 1, data.size
    assert_equal "ok", data[0]["name"]
  end

  # ----- Encoding detection -----

  def test_encoding_default_utf8
    doc = Scrapetor.parse("<html><body>plain ASCII</body></html>")
    assert_equal "UTF-8", doc.encoding
  end

  def test_encoding_strips_utf8_bom
    html = "\xEF\xBB\xBF<html><body>x</body></html>".dup.force_encoding("ASCII-8BIT")
    doc = Scrapetor.parse(html)
    assert_equal "UTF-8", doc.encoding
    refute doc.html_str.start_with?("\xEF\xBB\xBF")
  end

  def test_encoding_meta_charset_utf8
    html = '<html><head><meta charset="utf-8"></head><body>hi</body></html>'
    doc = Scrapetor.parse(html)
    assert_equal "UTF-8", doc.encoding
  end

  def test_encoding_meta_charset_windows1252
    # Bytes "\xa3" = £ in Windows-1252 / Latin-1
    html = ('<html><head><meta charset="windows-1252"></head><body>price ' + "\xa3" + '19</body></html>').dup.force_encoding("ASCII-8BIT")
    doc = Scrapetor.parse(html)
    assert_equal "WINDOWS-1252", doc.encoding
    assert doc.html_str.encoding == ::Encoding::UTF_8
    assert doc.html_str.include?("£"), "expected pound sign transcoded: got #{doc.html_str.inspect}"
  end

  def test_encoding_http_equiv_content_type
    html = '<html><head><meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1"></head><body>x</body></html>'
    doc = Scrapetor.parse(html)
    assert_equal "WINDOWS-1252", doc.encoding
  end

  def test_encoding_invalid_utf8_bytes_dont_crash
    html = "<html><body>" + "\xc3\x28".dup.force_encoding("ASCII-8BIT") + "ok</body></html>"
    doc = Scrapetor.parse(html)
    assert doc.html_str.valid_encoding?, "transcoded output must be valid UTF-8"
  end

  # ----- Hardening: adversarial / malformed HTML -----

  def test_native_handles_truncated_tag
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class="card"><span class="t">text<'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    # Should not segfault or hang. May return partial or empty results.
    result = Scrapetor.extract_native(html, schema)
    assert_kind_of Hash, result
    assert_kind_of Array, result[:xs]
  end

  def test_native_handles_unclosed_quote
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class="card"><a href="/a>broken<span class="t">x</span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_kind_of Hash, result
  end

  def test_native_handles_mismatched_close
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class="card"><span class="t">x</span></div></span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "x", result[:xs][0][:t]
  end

  def test_native_handles_deep_nesting
    skip "native ext not built" unless Scrapetor::Native.available?
    # 800 levels of nesting (under our 1024 limit)
    depth = 800
    html = '<div class="card">' + ('<div>' * depth) + '<span class="t">deep</span>' + ('</div>' * depth) + '</div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal "deep", result[:xs][0][:t]
  end

  def test_native_handles_extreme_nesting_gracefully
    skip "native ext not built" unless Scrapetor::Native.available?
    # 2000 levels — exceeds MAX_STACK; should not crash
    html = '<div class="card">' + ('<div>' * 2000) + 'text' + ('</div>' * 2000) + '</div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: "div", multi: false
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_kind_of Hash, result
  end

  def test_native_handles_empty_input
    skip "native ext not built" unless Scrapetor::Native.available?
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    assert_equal({xs: []}, Scrapetor.extract_native("", schema))
  end

  def test_native_handles_no_matches
    skip "native ext not built" unless Scrapetor::Native.available?
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    html = "<html><body><p>no cards here</p></body></html>"
    assert_equal({xs: []}, Scrapetor.extract_native(html, schema))
  end

  def test_native_handles_comments_correctly
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class="card"><!-- ignored --><span class="t">x</span><!-- <span class="t">no</span> --></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    assert_equal "x", Scrapetor.extract_native(html, schema)[:xs][0][:t]
  end

  def test_native_handles_self_closing_attributes
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class="card"><img src="/a.png" alt="x"/><span class="t">y</span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :img, from: "img", attr: :src
        field :t,   from: ".t"
      end
    end
    r = Scrapetor.extract_native(html, schema)
    assert_equal "/a.png", r[:xs][0][:img]
    assert_equal "y", r[:xs][0][:t]
  end

  def test_native_handles_unquoted_attribute_values
    skip "native ext not built" unless Scrapetor::Native.available?
    html = '<div class=card><span class=t>x</span></div>'
    schema = Scrapetor.schema do
      repeated ".card", as: :xs do
        field :t, from: ".t"
      end
    end
    assert_equal "x", Scrapetor.extract_native(html, schema)[:xs][0][:t]
  end

  def test_native_handles_large_input
    skip "native ext not built" unless Scrapetor::Native.available?
    # 1 MB of repeated cards
    one_card = '<div class="c"><span class="t">x</span></div>'
    html = one_card * 20_000
    schema = Scrapetor.schema do
      repeated ".c", as: :xs do
        field :t, from: ".t"
      end
    end
    result = Scrapetor.extract_native(html, schema)
    assert_equal 20_000, result[:xs].size
  end

  # ----- Nokogiri compat surface -----

  def test_nokogiri_style_module_method
    doc = Scrapetor::HTML(HTML)
    assert_kind_of Scrapetor::Document, doc
    assert_equal 3, doc.css(".product-card").size
  end

  def test_parse_html_alias
    doc = Scrapetor.parse_html(HTML)
    assert_kind_of Scrapetor::Document, doc
  end

  def test_node_content_inner_text_aliases
    n = @doc.at(".title")
    assert_equal n.text, n.content
    assert_equal n.text, n.inner_text
  end

  def test_node_attributes_hash
    a = @doc.at("a")
    attrs = a.attributes
    assert_equal "/widget-a", attrs["href"]
    assert_includes a.keys, "href"
    assert_includes a.values, "/widget-a"
    assert a.has_attribute?("href")
    refute a.has_attribute?("nonexistent")
  end

  def test_node_each_attribute
    cards = @doc.css(".product-card")
    pairs = cards.first.each_attribute.to_a
    assert_includes pairs, ["class", "product-card"]
    assert_includes pairs, ["data-sku", "A1"]
  end

  def test_node_set_inner_text
    ns = @doc.css(".title")
    combined = ns.inner_text
    assert combined.include?("Widget A")
    assert combined.include?("Widget B")
    assert combined.include?("Widget C")
  end

  def test_node_set_css_nesting
    nested = @doc.css(".product-card").css(".price")
    assert_equal 3, nested.size
  end

  def test_node_set_to_html
    fragment = @doc.css(".product-card").to_html
    assert fragment.include?("product-card")
  end

  def test_document_title_body_head
    doc = Scrapetor.parse("<html><head><title>T</title></head><body><p>x</p></body></html>")
    assert_equal "T", doc.title
    assert doc.body
    assert doc.head
    assert doc.html?
    refute doc.xml?
    assert_equal [], doc.errors
  end

  def test_node_children_and_parent
    card = @doc.at(".product-card")
    titles_under = card.css(".title")
    assert_equal 1, titles_under.size
    title = titles_under.first
    assert_equal "h2", title.name
    parent = title.parent
    assert parent
    assert parent[:class].include?("product-card")
  end
end
