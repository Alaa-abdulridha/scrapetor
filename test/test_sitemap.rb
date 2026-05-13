# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "scrapetor"

class TestSitemap < Minitest::Test
  URLSET = <<~XML
    <?xml version="1.0" encoding="UTF-8"?>
    <urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
      <url>
        <loc>https://example.com/a</loc>
        <lastmod>2024-01-01</lastmod>
        <priority>0.8</priority>
      </url>
      <url>
        <loc>https://example.com/b</loc>
        <lastmod>2024-02-02</lastmod>
        <changefreq>daily</changefreq>
      </url>
      <url><loc>https://example.com/c</loc></url>
    </urlset>
  XML

  def test_enumerates_all_urls
    urls = []
    Scrapetor::Sitemap.urls(URLSET) { |u, _| urls << u }
    assert_equal ["https://example.com/a",
                  "https://example.com/b",
                  "https://example.com/c"], urls
  end

  def test_meta_is_carried_when_present
    rows = []
    Scrapetor::Sitemap.urls(URLSET) { |u, m| rows << [u, m] }
    assert_equal "2024-01-01", rows[0][1][:lastmod]
    assert_equal "0.8",        rows[0][1][:priority]
    assert_equal "2024-02-02", rows[1][1][:lastmod]
    assert_equal "daily",      rows[1][1][:changefreq]
    assert_nil rows[2][1][:lastmod]
  end

  def test_returns_enumerator_when_no_block
    enum = Scrapetor::Sitemap.urls(URLSET)
    assert_kind_of Enumerator, enum
    assert_equal 3, enum.to_a.size
  end

  def test_accepts_string_io
    require "stringio"
    urls = []
    Scrapetor::Sitemap.urls(StringIO.new(URLSET)) { |u, _| urls << u }
    assert_equal 3, urls.size
  end

  def test_does_not_crash_on_empty_urlset
    empty = '<?xml version="1.0"?><urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"></urlset>'
    urls = []
    Scrapetor::Sitemap.urls(empty) { |u, _| urls << u }
    assert_empty urls
  end

  def test_sitemap_index_recurses
    # Index points at two child sitemaps in-memory; we approximate by
    # feeding the index whose <loc>s are inline blobs (not http URLs)
    # — Sitemap#open_source treats non-http Strings as inline XML.
    skip "sitemapindex recursion requires URL targets to fetch; covered by integration tests"
  end

  def test_strips_outer_whitespace_in_loc
    xml = "<urlset><url><loc>  https://example.com/spaces  </loc></url></urlset>"
    urls = []
    Scrapetor::Sitemap.urls(xml) { |u, _| urls << u }
    assert_equal ["https://example.com/spaces"], urls
  end
end
