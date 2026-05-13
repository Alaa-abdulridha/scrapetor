# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
$LOAD_PATH.unshift File.expand_path("./support", __dir__)
require "minitest/autorun"
require "scrapetor"
require "local_server"

class TestSession < Minitest::Test
  def self.runnable_methods
    return [] unless Scrapetor::Fetcher.available?
    super
  end

  def setup
    @server = Scrapetor::TestSupport::LocalServer.new
  end

  def teardown
    @server.stop
    @session&.close
  end

  def test_default_headers_applied_to_every_request
    seen_accept = []
    @server.mount("/p") do |req, res|
      seen_accept << req["Accept-Language"]
      res.status = 200; res.body = "ok"
    end
    @session = Scrapetor::Session.new(
      cookies: false,
      headers: { "Accept-Language" => "fr-CA" },
    )
    @session.get(@server.url("/p"))
    @session.get(@server.url("/p"))
    assert_equal ["fr-CA", "fr-CA"], seen_accept
  end

  def test_cookie_jar_carries_across_requests
    @server.mount("/set") do |req, res|
      res.cookies << WEBrick::Cookie.new("sid", "ABC123")
      res.body = "set"; res.status = 200
    end
    @server.mount("/echo") do |req, res|
      res.body = req.cookies.map { |c| "#{c.name}=#{c.value}" }.join("; ")
      res.status = 200
    end
    @session = Scrapetor::Session.new(cookies: true)
    @session.get(@server.url("/set"))
    r = @session.get(@server.url("/echo"))
    assert_includes r[:body], "sid=ABC123"
  end

  def test_cookies_disabled
    @server.mount("/set") do |_, res|
      res.cookies << WEBrick::Cookie.new("sid", "X")
      res.body = "set"; res.status = 200
    end
    @server.mount("/echo") do |req, res|
      res.body = req.cookies.size.to_s; res.status = 200
    end
    @session = Scrapetor::Session.new(cookies: false)
    @session.get(@server.url("/set"))
    r = @session.get(@server.url("/echo"))
    assert_equal "0", r[:body]
  end

  def test_bearer_token_applied_by_default
    seen_auth = nil
    @server.mount("/p") do |req, res|
      seen_auth = req["Authorization"]
      res.body = "ok"; res.status = 200
    end
    @session = Scrapetor::Session.new(cookies: false, bearer_token: "tok-Z")
    @session.get(@server.url("/p"))
    assert_equal "Bearer tok-Z", seen_auth
  end

  def test_rate_limit_serialises_same_host_calls
    @server.mount("/p") { |_, res| res.status = 200; res.body = "ok" }
    @session = Scrapetor::Session.new(cookies: false, rate_limit: 0.1)
    require "benchmark"
    t = Benchmark.realtime do
      3.times { @session.get(@server.url("/p")) }
    end
    # 3 hits at 100ms gate: 1st free, then 2 × 100ms gates.
    assert_in_delta 0.2, t, 0.15
  end

  def test_session_fetch_returns_parsed_document
    @server.mount("/p") do |_, res|
      res.status = 200; res["Content-Type"] = "text/html; charset=utf-8"
      res.body = "<html><body><h1>title</h1></body></html>"
    end
    @session = Scrapetor::Session.new(cookies: false)
    doc = @session.fetch(@server.url("/p"))
    assert_equal "title", doc.at_css("h1").text
  end

  def test_session_fetch_raises_on_non_2xx
    @server.mount("/p") { |_, res| res.status = 500; res.body = "bad" }
    @session = Scrapetor::Session.new(cookies: false)
    assert_raises(Scrapetor::Fetcher::FetchError) do
      @session.fetch(@server.url("/p"))
    end
  end

  def test_parallel_get_runs_under_session_defaults
    seen_ua = []
    @server.mount("/p") do |req, res|
      seen_ua << req["User-Agent"]
      res.status = 200; res.body = "ok"
    end
    @session = Scrapetor::Session.new(cookies: false, user_agent: "MyBot/1.0")
    urls = [@server.url("/p")] * 4
    @session.parallel_get(urls, threads: 4)
    assert_equal ["MyBot/1.0"] * 4, seen_ua
  end
end
