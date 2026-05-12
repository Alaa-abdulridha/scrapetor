# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

class TestMicrodataPageType < Minitest::Test
  # ----- Microdata -----

  def test_simple_microdata_extraction
    html = <<~HTML
      <html><body>
        <div itemscope itemtype="https://schema.org/Product">
          <h1 itemprop="name">Widget</h1>
          <span itemprop="price">19.99</span>
          <meta itemprop="brand" content="Acme">
        </div>
      </body></html>
    HTML
    items = Scrapetor.parse(html).microdata
    assert_equal 1, items.size
    item = items[0]
    assert_equal "https://schema.org/Product", item["type"]
    assert_equal "Widget", item["properties"]["name"]
    assert_equal "19.99", item["properties"]["price"]
    assert_equal "Acme", item["properties"]["brand"]
  end

  def test_nested_microdata_items
    html = <<~HTML
      <html><body>
        <div itemscope itemtype="https://schema.org/Product">
          <span itemprop="name">Widget</span>
          <div itemprop="offers" itemscope itemtype="https://schema.org/Offer">
            <span itemprop="price">19.99</span>
            <span itemprop="priceCurrency">USD</span>
          </div>
        </div>
      </body></html>
    HTML
    items = Scrapetor.parse(html).microdata
    offer = items[0]["properties"]["offers"]
    assert_kind_of Hash, offer
    assert_equal "https://schema.org/Offer", offer["type"]
    assert_equal "19.99", offer["properties"]["price"]
    assert_equal "USD",   offer["properties"]["priceCurrency"]
  end

  def test_microdata_repeated_property
    html = <<~HTML
      <html><body>
        <div itemscope itemtype="https://schema.org/Recipe">
          <span itemprop="recipeIngredient">flour</span>
          <span itemprop="recipeIngredient">sugar</span>
          <span itemprop="recipeIngredient">butter</span>
        </div>
      </body></html>
    HTML
    item = Scrapetor.parse(html).microdata[0]
    assert_equal %w[flour sugar butter], item["properties"]["recipeIngredient"]
  end

  def test_microdata_attribute_based_values
    html = <<~HTML
      <html><body>
        <div itemscope itemtype="https://schema.org/Event">
          <time itemprop="startDate" datetime="2026-12-01T19:00">Dec 1</time>
          <a itemprop="url" href="/event/1">Details</a>
          <img itemprop="image" src="/img/e.png">
          <meta itemprop="organizer" content="Acme">
        </div>
      </body></html>
    HTML
    p = Scrapetor.parse(html).microdata[0]["properties"]
    assert_equal "2026-12-01T19:00", p["startDate"]
    assert_equal "/event/1",         p["url"]
    assert_equal "/img/e.png",       p["image"]
    assert_equal "Acme",             p["organizer"]
  end

  # ----- RDFa -----

  def test_simple_rdfa
    html = <<~HTML
      <html><body>
        <div typeof="schema:Product" about="#widget">
          <span property="schema:name">Widget</span>
          <span property="schema:price" content="19.99">$19.99</span>
        </div>
      </body></html>
    HTML
    items = Scrapetor.parse(html).rdfa
    assert_equal 1, items.size
    assert_equal "schema:Product", items[0]["type"]
    assert_equal "Widget",         items[0]["properties"]["schema:name"]
    assert_equal "19.99",          items[0]["properties"]["schema:price"]
  end

  # ----- Page-type detection -----

  def test_product_page_via_jsonld
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">{"@context":"https://schema.org","@type":"Product","name":"X"}</script>
      </head><body><h1>X</h1></body></html>
    HTML
    assert_equal :product_page, Scrapetor.parse(html).page_type
  end

  def test_article_via_jsonld
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">{"@type":"NewsArticle","headline":"H"}</script>
      </head><body><article><h1>H</h1></article></body></html>
    HTML
    assert_equal :article, Scrapetor.parse(html).page_type
  end

  def test_product_listing_via_jsonld
    html = <<~HTML
      <html><head>
        <script type="application/ld+json">[
          {"@type":"ItemList"},
          {"@type":"Product","name":"A"}
        ]</script>
      </head><body></body></html>
    HTML
    assert_equal :product_listing, Scrapetor.parse(html).page_type
  end

  def test_product_page_via_opengraph
    html = <<~HTML
      <html><head>
        <meta property="og:type" content="product">
      </head><body></body></html>
    HTML
    assert_equal :product_page, Scrapetor.parse(html).page_type
  end

  def test_article_via_opengraph
    html = <<~HTML
      <html><head>
        <meta property="og:type" content="article">
      </head><body></body></html>
    HTML
    assert_equal :article, Scrapetor.parse(html).page_type
  end

  def test_product_listing_structural
    cards = (1..8).map { '<div class="product-card"><h2>X</h2></div>' }.join
    html = "<html><body>#{cards}</body></html>"
    assert_equal :product_listing, Scrapetor.parse(html).page_type
  end

  def test_article_structural
    body = "<p>" + ("a long sentence with many words. " * 60) + "</p>"
    html = "<html><body><article><h1>T</h1>#{body}</article></body></html>"
    assert_equal :article, Scrapetor.parse(html).page_type
  end

  def test_search_results_structural
    html = '<html><body><form role="search"><input type="search"></form><div class="search-result">x</div></body></html>'
    assert_equal :search_results, Scrapetor.parse(html).page_type
  end

  def test_unknown_when_no_signals
    html = "<html><body><p>just plain text</p></body></html>"
    assert_equal :unknown, Scrapetor.parse(html).page_type
  end
end
