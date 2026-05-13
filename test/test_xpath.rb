# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "scrapetor"

class TestXPath < Minitest::Test
  HTML = <<~HTML
    <!DOCTYPE html>
    <html>
      <head>
        <title>Listing</title>
      </head>
      <body>
        <header>
          <a href="/" rel="home">Home</a>
        </header>
        <main id="results">
          <div class="card" data-rank="1">
            <h2>Card A</h2>
            <a href="/a" rel="next" data-id="100">Next</a>
            <span class="price">$10</span>
          </div>
          <div class="card featured" data-rank="2">
            <h2>Card B</h2>
            <span class="price">$20</span>
          </div>
          <div class="ad" data-rank="3">
            <h2>Sponsored</h2>
            <span class="price">--</span>
          </div>
        </main>
      </body>
    </html>
  HTML

  def setup
    @doc = Scrapetor.parse(HTML, base_url: "https://x.test/")
  end

  # --- node-set selection ----

  def test_descendant_any_element_by_tag
    assert_equal 3, @doc.xpath("//div").size
  end

  def test_descendant_with_attribute_equality_filter
    nodes = @doc.xpath("//div[@class='card']")
    assert_equal 1, nodes.size
    assert_equal "Card A", nodes.first.at_css("h2").text
  end

  def test_descendant_with_contains_filter
    nodes = @doc.xpath("//div[contains(@class,'card')]")
    assert_equal 2, nodes.size
  end

  def test_descendant_with_starts_with_filter
    nodes = @doc.xpath("//a[starts-with(@rel,'ne')]")
    assert_equal 1, nodes.size
    assert_equal "Next", nodes.first.text
  end

  def test_attribute_presence_filter
    assert_equal 3, @doc.xpath("//div[@data-rank]").size
    assert_equal 0, @doc.xpath("//div[@nonexistent]").size
  end

  def test_position_predicate_1_based
    nodes = @doc.xpath("//div[1]/h2")
    assert_equal "Card A", nodes.first.text
    assert_equal 1,        nodes.size
  end

  # --- attribute / text access ----

  def test_attribute_access_returns_strings
    hrefs = @doc.xpath("//a/@href")
    assert_equal ["/", "/a"], hrefs
  end

  def test_text_node_access_returns_strings
    titles = @doc.xpath("//div/h2/text()")
    assert_equal ["Card A", "Card B", "Sponsored"], titles.map(&:strip)
  end

  def test_text_equality_predicate
    nodes = @doc.xpath("//a[text()='Next']")
    assert_equal 1, nodes.size
    assert_equal "/a", nodes.first[:href]
  end

  # --- any-element + id ----

  def test_star_by_id
    nodes = @doc.xpath("//*[@id='results']")
    assert_equal 1, nodes.size
  end

  # --- scoped from node ----

  def test_scoped_to_node_uses_node_as_context
    main_node = @doc.at_css("#results")
    nodes = main_node.xpath(".//h2")
    assert_equal 3, nodes.size
  end

  # --- error handling for unsupported syntax ----

  def test_union_raises_unsupported
    assert_raises(Scrapetor::XPath::UnsupportedError) do
      @doc.xpath("//div | //h2")
    end
  end

  def test_bogus_predicate_raises
    assert_raises(Scrapetor::XPath::UnsupportedError) do
      @doc.xpath("//div[1 + 1]")
    end
  end

  def test_at_xpath_returns_first
    node = @doc.at_xpath("//div")
    refute_nil node
    assert_equal "Card A", node.at_css("h2").text
  end

  def test_at_xpath_returns_nil_when_empty
    assert_nil @doc.at_xpath("//nonexistent")
  end

  def test_chained_descendant
    nodes = @doc.xpath("//main[@id='results']//h2")
    assert_equal 3, nodes.size
  end

  def test_relative_path_from_node
    # h2 inside Card A using "h2" relative to the first card.
    first = @doc.at_xpath("//div[1]")
    h2s = first.xpath("h2")
    assert_equal 1, h2s.size
    assert_equal "Card A", h2s.first.text
  end
end
