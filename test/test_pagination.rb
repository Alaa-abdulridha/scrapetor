# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
$LOAD_PATH.unshift File.expand_path("./support", __dir__)
require "minitest/autorun"
require "scrapetor"

begin
  require "webrick"
  require "local_server"
  TEST_PAG_WEBRICK = true
rescue LoadError
  TEST_PAG_WEBRICK = false
end

class TestPagination < Minitest::Test
  def self.runnable_methods
    return [] unless TEST_PAG_WEBRICK
    super
  end

  def setup
    @server = Scrapetor::TestSupport::LocalServer.new
  end

  def teardown
    @server.stop
  end

  # ---------- next-link detection from a parsed DOM (no network) ----------

  def test_detects_link_rel_next_in_head
    doc = Scrapetor.parse(<<~HTML, base_url: "https://example.com/p1")
      <html>
        <head><link rel="next" href="/p2"></head>
        <body></body>
      </html>
    HTML
    assert_equal "https://example.com/p2",
                 Scrapetor::Pagination.next_page_url(doc, "https://example.com/p1")
  end

  def test_detects_anchor_rel_next
    doc = Scrapetor.parse(<<~HTML, base_url: "https://example.com/p1")
      <html><body>
        <a href="/p2" rel="next">next</a>
      </body></html>
    HTML
    assert_equal "https://example.com/p2",
                 Scrapetor::Pagination.next_page_url(doc, "https://example.com/p1")
  end

  def test_link_rel_takes_priority_over_anchor
    doc = Scrapetor.parse(<<~HTML, base_url: "https://example.com/p1")
      <html>
        <head><link rel="next" href="/from-link-rel"></head>
        <body><a rel="next" href="/from-anchor">n</a></body>
      </html>
    HTML
    assert_equal "https://example.com/from-link-rel",
                 Scrapetor::Pagination.next_page_url(doc, "https://example.com/p1")
  end

  def test_relative_href_resolved_against_current_url
    doc = Scrapetor.parse('<a rel="next" href="page-3">n</a>')
    assert_equal "https://x.com/list/page-3",
                 Scrapetor::Pagination.next_page_url(doc, "https://x.com/list/")
  end

  def test_returns_nil_when_no_next
    doc = Scrapetor.parse("<html><body>last page</body></html>")
    assert_nil Scrapetor::Pagination.next_page_url(doc, "https://x/")
  end

  def test_custom_selector_picks_up_anchor_without_rel
    doc = Scrapetor.parse(<<~HTML)
      <html><body>
        <div class="paging"><a href="/next-page" class="goto">Next &raquo;</a></div>
      </body></html>
    HTML
    assert_equal "https://x.com/next-page",
                 Scrapetor::Pagination.next_page_url(doc, "https://x.com/", ".paging a.goto")
  end

  # ---------- iterating real pages over a WEBrick server ----------

  def test_each_page_walks_until_no_next_link
    %w[1 2 3].each do |i|
      next_attr = i == "3" ? "" : "<a rel='next' href='/p#{i.to_i + 1}'>n</a>"
      @server.mount("/p#{i}") do |_, res|
        res["Content-Type"] = "text/html; charset=utf-8"
        res.body = "<html><body><h1>page#{i}</h1>#{next_attr}</body></html>"
        res.status = 200
      end
    end
    titles = []
    Scrapetor::Pagination.each_page(@server.url("/p1")) do |doc, _url|
      titles << doc.at_css("h1").text
    end
    assert_equal %w[page1 page2 page3], titles
  end

  def test_max_pages_limits_iteration
    %w[1 2 3 4 5].each do |i|
      nxt = i == "5" ? "" : "<a rel='next' href='/q#{i.to_i + 1}'>n</a>"
      @server.mount("/q#{i}") do |_, res|
        res["Content-Type"] = "text/html; charset=utf-8"
        res.body = "<html><body><h1>q#{i}</h1>#{nxt}</body></html>"
        res.status = 200
      end
    end
    titles = []
    Scrapetor::Pagination.each_page(@server.url("/q1"), max_pages: 3) do |doc, _|
      titles << doc.at_css("h1").text
    end
    assert_equal %w[q1 q2 q3], titles
  end

  def test_self_loop_detected_and_aborted
    # /loop has rel=next pointing at itself. The walker should yield
    # exactly once and stop on the second visit.
    @server.mount("/loop") do |_, res|
      url = @server.url("/loop")
      res.body = "<html><body><h1>loop</h1><a rel='next' href='#{url}'>n</a></body></html>"
      res["Content-Type"] = "text/html; charset=utf-8"
      res.status = 200
    end
    n = 0
    Scrapetor::Pagination.each_page(@server.url("/loop")) { |_, _| n += 1 }
    assert_equal 1, n
  end

  def test_returns_enumerator_without_block
    enum = Scrapetor::Pagination.each_page("https://example.com/")
    assert_kind_of Enumerator, enum
  end

  def test_top_level_shorthand_delegates
    doc = Scrapetor.parse('<a rel="next" href="/x">n</a>')
    assert_equal "https://h/x",
                 Scrapetor::Pagination.next_page_url(doc, "https://h/")
  end
end
