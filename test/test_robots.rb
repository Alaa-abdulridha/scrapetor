# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "scrapetor"

class TestRobots < Minitest::Test
  ROBOTS = <<~ROBOTS
    User-agent: *
    Disallow: /private/
    Disallow: /admin
    Allow: /admin/public

    User-agent: BadBot
    Disallow: /

    Sitemap: https://example.com/sitemap.xml
    Sitemap: https://example.com/news-sitemap.xml
  ROBOTS

  def setup
    @r = Scrapetor::Robots.new(ROBOTS, user_agent: "scrapetor")
  end

  def test_allows_unmatched_paths
    assert @r.allowed?("/public")
    assert @r.allowed?("/")
    assert @r.allowed?("/index.html")
  end

  def test_disallows_prefix_match
    refute @r.allowed?("/private/")
    refute @r.allowed?("/private/secrets.html")
  end

  def test_longest_match_wins
    refute @r.allowed?("/admin")
    refute @r.allowed?("/admin/other")
    assert @r.allowed?("/admin/public"),
           "more specific Allow: /admin/public should beat Disallow: /admin"
  end

  def test_full_url_accepted
    assert @r.allowed?("https://example.com/public")
    refute @r.allowed?("https://example.com/private/x")
  end

  def test_specific_user_agent_group_overrides_star
    bad = Scrapetor::Robots.new(ROBOTS, user_agent: "BadBot/1.0")
    refute bad.allowed?("/anything")
    refute bad.allowed?("/")
  end

  def test_unknown_agent_falls_back_to_star
    other = Scrapetor::Robots.new(ROBOTS, user_agent: "OtherCrawler")
    assert other.allowed?("/public")
    refute other.allowed?("/private/")
  end

  def test_sitemaps_extracted
    assert_equal ["https://example.com/sitemap.xml",
                  "https://example.com/news-sitemap.xml"],
                 @r.sitemaps
  end

  def test_wildcard_and_anchor
    r = Scrapetor::Robots.new(<<~R, user_agent: "*")
      User-agent: *
      Disallow: /*.pdf$
      Disallow: /search?
    R
    refute r.allowed?("/files/x.pdf")
    refute r.allowed?("/search?q=x")
    assert r.allowed?("/files/x.pdf.html")  # $ anchor — pdf must end the path
    assert r.allowed?("/searchresults")     # ? boundary preserved
  end

  def test_crawl_delay_attaches_to_current_group
    r = Scrapetor::Robots.new(<<~R, user_agent: "scrapetor")
      User-agent: *
      Crawl-delay: 2
      Disallow:
    R
    assert_in_delta 2.0, r.crawl_delay, 0.0001
  end

  def test_empty_disallow_means_allow_all
    r = Scrapetor::Robots.new(<<~R, user_agent: "x")
      User-agent: *
      Disallow:
    R
    assert r.allowed?("/anything")
  end

  def test_comments_and_blank_lines_ignored
    r = Scrapetor::Robots.new(<<~R, user_agent: "x")
      # leading comment
      User-agent: *

      Disallow: /private  # inline comment

      Allow: /public
    R
    refute r.allowed?("/private/x")
    assert r.allowed?("/public/x")
  end
end
