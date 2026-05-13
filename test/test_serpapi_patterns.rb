# frozen_string_literal: true

require "minitest/autorun"
require "scrapetor"

# Locks in the CSS selector + mutation API surface that real production
# scrapers depend on. Originally driven by a SerpApi v0.1.0 audit that
# surfaced ArgumentError crashes on every leading-`>` selector and a
# missing `inner_html=` setter on the native element wrapper. Each form
# below maps to actual call sites in production parser code.
class TestSerpApiPatterns < Minitest::Test
  HTML = <<~HTML.freeze
    <html><body>
      <div id="main">
        <div class="g featured">
          <h3 class="LC20lb">Result 1</h3>
          <a href="/r/1">link</a>
          <div class="sitelinks">
            <a href="/s/1">sub</a>
          </div>
        </div>
        <div class="g">
          <h3 class="LC20lb">Result 2</h3>
          <a href="/r/2">link</a>
        </div>
        <div class="g">
          <h3 class="LC20lb">Result 3</h3>
          <a href="/r/3">link</a>
          <p class="paa-q">
            <span class="q">A question?</span>
          </p>
        </div>
      </div>
      <ul class="pagination">
        <li><a href="/p/1">1</a></li>
        <li><a href="/p/2">2</a></li>
        <li><a href="/p/3">3</a></li>
      </ul>
    </body></html>
  HTML

  def setup
    @doc = Scrapetor::HTML(HTML)
  end

  # ----- pseudo-classes ---------------------------------------------

  def test_has_with_simple_inner
    assert_equal 1, @doc.css("div.g:has(.sitelinks)").size
  end

  def test_has_with_class_inner
    assert_equal 1, @doc.css("div.g:has(.paa-q)").size
  end

  def test_not_class
    # 3 .g divs; 1 has .featured, 2 don't
    assert_equal 2, @doc.css("div.g:not(.featured)").size
  end

  def test_not_with_pseudo_inner
    # All .q spans that aren't :empty
    assert_equal 1, @doc.css(".q:not(:empty)").size
  end

  def test_is_alternatives
    assert_equal 3, @doc.css(":is(.g, .gnope)").size
  end

  def test_first_child_scope_relative
    # `> :first-child` from inside #main: each child div counts its own
    # first-child relative to itself; doc.css('#main > :first-child')
    # picks just the first child of #main.
    assert_equal 1, @doc.css("#main > :first-child").size
  end

  def test_first_child
    assert_equal 1, @doc.css("#main div.g:first-child").size
  end

  def test_last_child
    assert_equal 1, @doc.css("#main div.g:last-child").size
  end

  def test_nth_child
    assert_equal 1, @doc.css("#main div.g:nth-child(2)").size
  end

  def test_nth_of_type
    assert_equal 2, @doc.css("#main div.g:nth-of-type(odd)").size
  end

  def test_first_of_type
    assert_equal 1, @doc.css("#main div.g:first-of-type").size
  end

  def test_last_of_type
    assert_equal 1, @doc.css("#main div.g:last-of-type").size
  end

  # ----- pseudo-elements --------------------------------------------

  def test_text_pseudo_element
    texts = @doc.css("h3.LC20lb::text")
    assert_equal ["Result 1", "Result 2", "Result 3"], texts
  end

  def test_attr_pseudo_element
    hrefs = @doc.css("#main > .g > a::attr(href)")
    assert_equal ["/r/1", "/r/2", "/r/3"], hrefs
  end

  # ----- leading combinators ----------------------------------------

  def test_leading_child_combinator_in_has
    # `:has(> .x)` — direct child only, not all descendants.
    assert_equal 1, @doc.css("div.g:has(> .sitelinks)").size
  end

  def test_leading_child_combinator_at_top
    # `node.css('> a')` from .sitelinks scope. The first .sitelinks
    # has one direct child <a>.
    node = @doc.at(".sitelinks")
    assert_equal 1, node.css("> a").size
  end

  def test_leading_combinator_in_has_with_nth_of_type
    # The crash that broke google_light: `> .x:nth-of-type(n)` inside :has.
    # Should parse cleanly and run without raising.
    @doc.css("div.g:has(> a:nth-of-type(1))").to_a
  end

  # ----- mutation API on Native::Element ----------------------------

  def test_inner_html_assignment
    doc = Scrapetor::HTML(HTML)
    node = doc.at("h3.LC20lb")
    node.inner_html = "<b>new</b>"
    assert_match(/<b>new<\/b>/, node.inner_html)
  end

  def test_attribute_assignment
    doc = Scrapetor::HTML(HTML)
    node = doc.at("div.g")
    node["data-test"] = "yes"
    assert_equal "yes", node["data-test"]
  end

  def test_content_assignment
    doc = Scrapetor::HTML(HTML)
    node = doc.at("h3.LC20lb")
    node.content = "Changed"
    assert_equal "Changed", node.text
  end

  def test_add_child
    doc = Scrapetor::HTML(HTML)
    doc.at("#main").add_child("<div class='added'>X</div>")
    assert_equal 1, doc.css(".added").size
  end

  def test_remove
    doc = Scrapetor::HTML(HTML)
    doc.at(".featured").remove
    refute doc.css(".featured").any?
  end

  def test_node_set_bulk_remove
    doc = Scrapetor::HTML(HTML)
    doc.css("li").remove
    assert_equal 0, doc.css("li").size
  end

  def test_replace
    doc = Scrapetor::HTML("<div><p>x</p></div>")
    doc.at("p").replace("<span>y</span>")
    assert_equal 1, doc.css("span").size
    assert_equal 0, doc.css("p").size
  end

  def test_wrap
    doc = Scrapetor::HTML("<div><p>x</p></div>")
    doc.at("p").wrap("<section/>")
    assert_equal 1, doc.css("section p").size
  end

  def test_before_after
    doc = Scrapetor::HTML(HTML)
    n = doc.at("h3.LC20lb")
    n.before("<i>i</i>")
    n.after("<u>u</u>")
    assert doc.css("i").any?
    assert doc.css("u").any?
  end

  # ----- Node-level XPath returns empty rather than raising ---------

  def test_node_xpath_empty
    node = @doc.at("div.g")
    result = node.xpath(".//nothing")
    assert_respond_to result, :size
    assert_equal 0, result.size
  end

  def test_node_at_xpath_nil
    assert_nil @doc.at("div.g").at_xpath(".//nothing")
  end

  # ----- batch_css amortises Ruby overhead --------------------------

  def test_batch_css_parallel_results
    results = @doc.batch_css(["div.g", ".pagination li", "h3.LC20lb::text"])
    assert_equal 3, results.first.size
    assert_equal 3, results[1].size
    assert_equal ["Result 1", "Result 2", "Result 3"], results[2]
  end

  def test_extract_css_hash
    out = @doc.extract_css(titles: "h3::text", links: "a::attr(href)")
    assert_equal ["Result 1", "Result 2", "Result 3"], out[:titles]
    assert out[:links].include?("/r/1")
  end

  # ----- NodeSet#css with ::text returns Array of strings -----------

  def test_node_set_css_with_text_pseudo
    # Was crashing with "undefined method `text' for #<String>" because
    # NodeSet#css wrapped strings into Node.new(@doc, str) and the next
    # .text call exploded. Now passes the strings through as an Array.
    result = @doc.css("div.g").css("h3.LC20lb::text")
    assert_kind_of Array, result
    assert(result.all? { |x| x.is_a?(String) })
    assert_equal ["Result 1", "Result 2", "Result 3"], result
  end

  def test_node_set_css_with_attr_pseudo
    # Each .g has its top result href + (in some) a nested sitelink.
    result = @doc.css("#main > .g").css("a::attr(href)")
    assert_kind_of Array, result
    assert_includes result, "/r/1"
    assert_includes result, "/r/2"
    assert_includes result, "/r/3"
    assert(result.all? { |x| x.is_a?(String) })
  end

  # ----- Nokogiri-compat: doc.at(sel, ns_or_handler) ----------------

  def test_doc_at_accepts_second_arg
    # Some legacy code (Bing's events_results parser among others)
    # calls `doc.at(sel, namespaces_hash)`. The second arg only matters
    # for XPath, so the CSS path must accept and ignore it instead of
    # raising ArgumentError.
    result = @doc.at("h3.LC20lb", {})
    refute_nil result
    assert_equal "Result 1", result.text.strip
  end

  def test_node_css_accepts_second_arg
    n = @doc.at(".g")
    result = n.css("h3", {})
    assert_kind_of Scrapetor::NodeSet, result
  end

  # ----- native remove works on parser-divergent HTML ---------------

  def test_native_remove_does_not_fall_back_to_dom
    # Used to raise NotImplementedError when the native arena and the
    # Ruby Dom view disagreed on whitespace text-node placement.
    # Native remove mutates the arena in place via the new
    # dom_node_remove path — no cross-DOM lookup needed.
    doc = Scrapetor::HTML("<div>  \n  <p>hi</p>  \n  <p>bye</p></div>")
    doc.at("p").remove
    assert_equal 1, doc.css("p").size
  end

  def test_native_remove_then_query
    doc = Scrapetor::HTML(HTML)
    doc.css("a").each(&:remove)
    assert_equal 0, doc.css("a").size
    # h3s still there
    assert_equal 3, doc.css("h3").size
  end

  # ----- transparent fallback never raises --------------------------

  def test_compile_never_raises_on_valid_css
    # Selectors that previously raised ArgumentError now parse cleanly.
    [
      "div:has(> .child)",
      "> a",
      "+ .next",
      "~ .later",
      "a:nth-of-type(2n+1)",
      ":not(:empty)",
      "p::text",
      "a::attr(href)",
      "div.a, div.b, div.c",
      ":is(.x, .y, .z)",
      ".a:has(.b:not(.c))"
    ].each do |sel|
      @doc.css(sel)  # must not raise
    end
  end
end
