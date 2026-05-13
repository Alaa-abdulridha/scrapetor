# frozen_string_literal: true

require "minitest/autorun"
require "scrapetor"

# Locks in the CSS selector + mutation API surface that real production
# scrapers depend on. Originally driven by a production audit that
# surfaced ArgumentError crashes on every leading-`>` selector and a
# missing `inner_html=` setter on the native element wrapper. Each form
# below maps to actual call sites in production parser code.
class TestProductionPatterns < Minitest::Test
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

  # ----- ::text results respond to .text / .content / .get ---------

  def test_text_pseudo_result_responds_to_text
    # Real-world parser code chains `.first.text` or `.at(::text).text`
    # onto the result expecting Nokogiri/Scrapy compatibility. The
    # TextNode wrapper is a String subclass that also exposes the
    # Node-style accessors, so both shapes work.
    title = @doc.at("h3.LC20lb::text")
    assert_kind_of String, title
    assert_equal "Result 1", title.text
    assert_equal "Result 1", title.content
    assert_equal "Result 1", title.inner_text
    assert_equal "Result 1", title.get
    assert_equal ["Result 1"], title.getall
    assert_equal "Result 1", title  # String comparison still works
  end

  def test_text_pseudo_supports_node_predicates
    title = @doc.at("h3::text")
    refute title.element?
    assert title.text?
    assert_equal "#text", title.name
  end

  def test_node_at_text_returns_text_node
    n = @doc.at("div.g")
    result = n.at("h3.LC20lb::text")
    assert_kind_of String, result
    assert_equal "Result 1", result.text
  end

  def test_css_text_pseudo_first_text
    # The exact production crash:
    # `nodes.map { |n| ... n.css(".x::text").first.text ... }`
    first = @doc.css("h3.LC20lb::text").first
    assert_equal "Result 1", first.text
  end

  # ----- NodeSet#children ------------------------------------------

  def test_node_set_children
    # Bing's organic_results.rb:219 iterates `.children` on a NodeSet
    # — previously crashed with `undefined method 'children'`.
    children = @doc.css(".g").children
    assert_kind_of Scrapetor::NodeSet, children
    refute_empty children.to_a
  end

  def test_node_set_children_aggregate
    doc = Scrapetor::HTML("<ul><li>1</li><li>2</li></ul><ul><li>3</li></ul>")
    children = doc.css("ul").children
    # Three <li> elements total across both <ul>s.
    li_count = children.to_a.count { |c| c.respond_to?(:name) && c.name == "li" }
    assert_equal 3, li_count
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

  # An unclosed :is(...) — broken selector seen in the wild — must not
  # crash the parser. Treat the dangling tail as the pseudo arg.
  def test_unterminated_pseudo_is_tolerant
    sel = ':is(.thumb_link, a:not(:has(.x)):not([data-target=".y"]):has(img)'
    @doc.css(sel)
  end

  # Attribute values must be entity-decoded so JSON-in-attribute (e.g.
  # an image `m="{...}"` payload) round-trips through JSON.parse.
  def test_attribute_entities_decoded
    require "json"
    html = %q{<a class="iusc" m="{&quot;u&quot;:&quot;https://x/y.jpg&quot;,&quot;d&quot;:&quot;A &amp; B&quot;}">x</a>}
    doc = Scrapetor.parse(html)
    a = doc.at_css("a")
    raw = a["m"]
    parsed = JSON.parse(raw)
    assert_equal "https://x/y.jpg", parsed["u"]
    assert_equal "A & B", parsed["d"]
    assert_equal raw, a.attributes["m"]
    assert_equal raw, doc.css("a::attr(m)").first.to_s
  end

  def test_numeric_entity_decoded_in_attr
    doc = Scrapetor.parse(%q{<a t="hi&#39;there&#x26;you">x</a>})
    assert_equal "hi'there&you", doc.at_css("a")["t"]
  end

  # ----- batch API ------------------------------------------------

  BATCH_HTML = <<~HTML.freeze
    <html><body>
      <div class="result">
        <h2 class="title">First</h2>
        <span class="price">$10</span>
        <a href="/p/1">link 1</a>
      </div>
      <div class="result">
        <h2 class="title">Second</h2>
        <span class="price">$20</span>
        <a href="/p/2">link 2</a>
      </div>
    </body></html>
  HTML

  def test_document_extract_each
    doc = Scrapetor.parse(BATCH_HTML)
    rows = doc.extract_each(".result", {
      title: ".title::text",
      price: ".price::text",
      href:  "a::attr(href)",
    })
    assert_equal 2, rows.size
    assert_equal "First",  rows[0][:title].to_s
    assert_equal "$10",    rows[0][:price].to_s
    assert_equal "/p/1",   rows[0][:href].to_s
    assert_equal "Second", rows[1][:title].to_s
  end

  def test_nodeset_extract_chains_off_css
    doc = Scrapetor.parse(BATCH_HTML)
    rows = doc.css(".result").extract(title: ".title::text", price: ".price::text")
    assert_equal %w[First Second], rows.map { |r| r[:title].to_s }
  end

  def test_node_batch_css
    doc = Scrapetor.parse(BATCH_HTML)
    result = doc.at_css(".result")
    parts = result.batch_css([".title::text", ".price::text", "a::attr(href)"])
    assert_equal 3, parts.size
    assert_equal "First", parts[0].first.to_s
    assert_equal "$10",   parts[1].first.to_s
    assert_equal "/p/1",  parts[2].first.to_s
  end

  def test_node_extract
    doc = Scrapetor.parse(BATCH_HTML)
    result = doc.at_css(".result")
    hash = result.extract(title: ".title::text", price: ".price::text")
    assert_equal "First", hash[:title].to_s
    assert_equal "$10",   hash[:price].to_s
  end

  def test_node_extract_each
    doc = Scrapetor.parse(BATCH_HTML)
    body = doc.at_css("body")
    rows = body.extract_each(".result", title: ".title::text", href: "a::attr(href)")
    assert_equal 2, rows.size
    assert_equal "First", rows[0][:title].to_s
    assert_equal "/p/2",  rows[1][:href].to_s
  end

  def test_extract_each_element_results
    doc = Scrapetor.parse(BATCH_HTML)
    rows = doc.extract_each(".result", anchor: "a")
    assert_equal 2, rows.size
    # Element-shaped result; verify it responds to text + attrs.
    assert_equal "link 1", rows[0][:anchor].text
    assert_equal "/p/2",   rows[1][:anchor]["href"]
  end

  def test_case_insensitive_attribute_flag
    doc = Scrapetor.parse(%q{<a aria-label="Directions">x</a><a aria-label="DIRECTIONS">y</a>})
    assert_equal %w[x y], doc.css('[aria-label="directions" i]').map(&:text)
    assert_equal [], doc.css('[aria-label="directions"]').map(&:text)
  end

  def test_adjacent_sibling_combinator
    doc = Scrapetor.parse("<span class=a>a</span><span class=b>b</span><span class=c>c</span>")
    assert_equal ["b"], doc.css(".a + .b").map(&:text)
    assert_equal [],    doc.css(".a + .c").map(&:text)
  end

  def test_general_sibling_combinator
    doc = Scrapetor.parse("<span class=a>a</span><span class=b>b</span><span class=c>c</span>")
    assert_equal %w[b c], doc.css(".a ~ *").map(&:text)
    assert_equal ["c"],   doc.css(".a ~ .c").map(&:text)
  end

  def test_direct_text_pseudo
    doc = Scrapetor.parse("<p>before <em>nested</em> after</p>")
    direct = doc.css("p > ::text").map(&:to_s)
    deep   = doc.css("p ::text").map(&:to_s)
    assert_equal ["before  after"], direct
    assert_equal ["before nested after"], deep
  end

  def test_is_distribution_with_combinators
    doc = Scrapetor.parse(
      "<aside><div class=x>A</div></aside>" \
      "<main><div class=p><div class=x>B</div></div></main>"
    )
    matches = doc.css(":is(aside, main .p) .x").map(&:text)
    assert_equal %w[A B], matches
  end

  def test_nodeset_range_slice
    doc = Scrapetor.parse("<ul><li>a</li><li>b</li><li>c</li><li>d</li></ul>")
    lis = doc.css("li")
    assert_equal %w[b c d], lis[1..-1].map(&:text)
    assert_equal %w[b c],   lis[1, 2].map(&:text)
  end

  def test_heterogeneous_pseudo_groups
    doc = Scrapetor.parse(
      "<div><p class=snip>hello <em>world</em></p>" \
      "<span class=fallback>extra</span></div>"
    )
    sel = ".snip > ::text, .fallback"
    results = doc.css(sel).to_a
    refute_empty results
    # First match (snip direct text) should be a TextNode-equivalent string.
    assert_includes results.map(&:to_s), "hello "
  end
end
