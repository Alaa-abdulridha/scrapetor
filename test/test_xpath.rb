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

  # --- axis support ----

  AXIS_HTML = <<~HTML
    <!DOCTYPE html>
    <html>
      <body>
        <!-- top -->
        <dl id="spec">
          <dt>Price</dt><dd>$199</dd>
          <dt>Color</dt><dd>Red</dd>
          <dt>Size</dt><dd>M</dd>
        </dl>
        <section id="main">
          <article id="a1">
            <h2>Title</h2>
            <!-- inline -->
            <p><a id="link" href="/x">Link</a></p>
          </article>
        </section>
      </body>
    </html>
  HTML

  def axis_doc
    @axis_doc ||= Scrapetor.parse(AXIS_HTML)
  end

  def test_following_sibling_axis_filtered_by_tag
    # All <dd> after Price's <dt> — XPath returns every dd that follows
    # in document order, not just the immediately-adjacent one.
    nodes = axis_doc.xpath("//dt[text()='Price']/following-sibling::dd")
    assert_equal ["$199", "Red", "M"], nodes.map(&:text)
  end

  def test_following_sibling_axis_immediate_via_position
    nodes = axis_doc.xpath("//dt[text()='Price']/following-sibling::dd[1]")
    assert_equal 1, nodes.size
    assert_equal "$199", nodes.first.text
  end

  def test_preceding_sibling_axis_returns_document_order
    # First <dd> ($199): preceding siblings in document order are the
    # Price dt only. The second <dd> (Red) preceding siblings are
    # [Price, $199, Color] — verify ordering by tag-filter to dt.
    dt_nodes = axis_doc.xpath("//dd[text()='Red']/preceding-sibling::dt")
    assert_equal ["Price", "Color"], dt_nodes.map(&:text)
  end

  def test_ancestor_axis_filtered_by_tag
    nodes = axis_doc.xpath("//a[@id='link']/ancestor::section")
    assert_equal 1, nodes.size
    assert_equal "main", nodes.first["id"]
  end

  def test_ancestor_axis_any_returns_root_first
    nodes = axis_doc.xpath("//a[@id='link']/ancestor::*")
    assert_equal %w[html body section article p], nodes.map(&:name)
  end

  def test_ancestor_or_self_axis_includes_self_last
    nodes = axis_doc.xpath("//a[@id='link']/ancestor-or-self::*")
    assert_equal %w[html body section article p a], nodes.map(&:name)
  end

  def test_descendant_comment_node_test
    comments = axis_doc.xpath("//comment()")
    assert_equal 2, comments.size
    assert comments.all? { |c| c.comment? }
    assert_equal ["top", "inline"], comments.map { |c| c.text.strip }
  end

  def test_child_comment_node_test
    article = axis_doc.at_xpath("//article")
    comments = article.xpath("comment()")
    assert_equal 1, comments.size
    assert_equal "inline", comments.first.text.strip
  end

  def test_comment_node_predicates
    c = axis_doc.at_xpath("//comment()")
    refute c.element?
    assert c.comment?
    assert_equal "#comment", c.name
    assert_equal 8, c.node_type
  end

  def test_scoped_following_sibling_from_node
    price_dt = axis_doc.at_xpath("//dt[text()='Price']")
    nodes = price_dt.xpath("following-sibling::dd")
    assert_equal ["$199", "Red", "M"], nodes.map(&:text)
  end

  def test_unsupported_axis_raises
    assert_raises(Scrapetor::XPath::UnsupportedError) do
      axis_doc.xpath("//div/following::p")
    end
  end
end
