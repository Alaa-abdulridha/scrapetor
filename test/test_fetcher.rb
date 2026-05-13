# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
$LOAD_PATH.unshift File.expand_path("./support", __dir__)
require "minitest/autorun"
require "fileutils"
require "json"
require "scrapetor"
require "local_server"

# Skip the whole class when libcurl wasn't linked into the gem.
class TestFetcher < Minitest::Test
  def self.runnable_methods
    return [] unless Scrapetor::Fetcher.available?
    super
  end

  def setup
    @server = Scrapetor::TestSupport::LocalServer.new
  end

  def teardown
    @server.stop
    if @cache_dir && File.directory?(@cache_dir)
      FileUtils.rm_rf(@cache_dir)
    end
  end

  def test_get_returns_status_body_headers_final_url
    @server.mount("/x") do |_, res|
      res.status = 200
      res["Content-Type"] = "text/html; charset=utf-8"
      res.body = "<h1>ok</h1>"
    end
    r = Scrapetor::Fetcher.get(@server.url("/x"))
    assert_equal 200, r[:status]
    assert_equal "<h1>ok</h1>", r[:body]
    assert_includes r[:headers]["content-type"], "text/html"
    assert_equal @server.url("/x"), r[:final_url]
  end

  def test_features_advertises_codecs
    f = Scrapetor::Fetcher.features
    assert f.key?(:curl_version)
    assert_kind_of String, f[:accept_encoding]
  end

  def test_post_json
    captured = nil
    @server.mount("/p") do |req, res|
      captured = { method: req.request_method, body: req.body, ctype: req["content-type"] }
      res.status = 200; res.body = "ok"
    end
    Scrapetor::Fetcher.post(@server.url("/p"), json: { hello: 42 })
    assert_equal "POST", captured[:method]
    assert_equal "application/json", captured[:ctype]
    assert_equal({ "hello" => 42 }, JSON.parse(captured[:body]))
  end

  def test_post_form
    captured = nil
    @server.mount("/p") do |req, res|
      captured = { ctype: req["content-type"], body: req.body }
      res.status = 200; res.body = "ok"
    end
    Scrapetor::Fetcher.post(@server.url("/p"), form: { user: "a", pass: "b!c" })
    assert_equal "application/x-www-form-urlencoded", captured[:ctype]
    assert_includes captured[:body], "user=a"
    assert_includes captured[:body], "pass=b%21c"
  end

  def test_methods_put_patch_delete
    seen = []
    %w[/u /p /d].zip(%w[PUT PATCH DELETE]).each do |path, _verb|
      @server.mount(path) do |req, res|
        seen << req.request_method
        res.status = 200; res.body = "ok"
      end
    end
    Scrapetor::Fetcher.put(@server.url("/u"), body: "x=1")
    Scrapetor::Fetcher.patch(@server.url("/p"), body: "x=2")
    Scrapetor::Fetcher.delete(@server.url("/d"))
    assert_equal %w[PUT PATCH DELETE], seen
  end

  def test_head_has_empty_body
    @server.mount("/h") do |_, res|
      res.status = 200; res["x-custom"] = "yes"; res.body = "should-not-be-returned"
    end
    r = Scrapetor::Fetcher.head(@server.url("/h"))
    assert_equal 200, r[:status]
    assert_equal 0, r[:body].bytesize
    assert_equal "yes", r[:headers]["x-custom"]
  end

  def test_basic_auth_attaches_authorization_header
    seen_auth = nil
    @server.mount("/a") do |req, res|
      seen_auth = req["authorization"]
      res.status = 200; res.body = "ok"
    end
    Scrapetor::Fetcher.get(@server.url("/a"), basic_auth: "alice:wonderland")
    assert_match(/\ABasic\s/, seen_auth)
  end

  def test_bearer_token_attaches_authorization_header
    seen_auth = nil
    @server.mount("/a") do |req, res|
      seen_auth = req["authorization"]
      res.status = 200; res.body = "ok"
    end
    Scrapetor::Fetcher.get(@server.url("/a"), bearer_token: "tok-XYZ")
    assert_equal "Bearer tok-XYZ", seen_auth
  end

  def test_charset_transcode_iso_8859_1_to_utf8
    @server.mount("/lat") do |_, res|
      res.body = "Café résumé".encode("ISO-8859-1")
      res["Content-Type"] = "text/html; charset=ISO-8859-1"
      res.status = 200
    end
    r = Scrapetor::Fetcher.get(@server.url("/lat"))
    assert_equal "UTF-8", r[:body].encoding.name
    assert_equal "Café résumé", r[:body]
    assert_match(/utf-8/i, r[:headers]["content-type"])
  end

  def test_charset_transcode_can_be_disabled
    @server.mount("/lat") do |_, res|
      res.body = "Café".encode("ISO-8859-1")
      res["Content-Type"] = "text/html; charset=ISO-8859-1"
      res.status = 200
    end
    r = Scrapetor::Fetcher.get(@server.url("/lat"), transcode_utf8: false)
    refute_equal "Café", r[:body]
  end

  def test_gzip_response_is_decompressed_in_process
    require "zlib"
    require "stringio"
    sio = StringIO.new
    gz = Zlib::GzipWriter.new(sio); gz.write("hello gzip"); gz.close
    body = sio.string
    @server.mount("/g") do |_, res|
      res.body = body; res["Content-Encoding"] = "gzip"; res.status = 200
    end
    r = Scrapetor::Fetcher.get(@server.url("/g"))
    assert_equal "hello gzip", r[:body]
    refute r[:headers].key?("content-encoding")
  end

  def test_5xx_status_raises_fetch_error_by_default
    @server.mount("/e") { |_, res| res.status = 500; res.body = "bad" }
    e = assert_raises(Scrapetor::Fetcher::FetchError) do
      Scrapetor::Fetcher.fetch(@server.url("/e"))
    end
    assert_equal 500, e.status
  end

  def test_raise_for_status_false_returns_non_2xx
    @server.mount("/e") { |_, res| res.status = 503; res.body = "down" }
    r = Scrapetor::Fetcher.get(@server.url("/e"))   # .get never raises on status
    assert_equal 503, r[:status]
  end

  def test_retry_succeeds_after_transient_5xx
    attempts = 0; mu = Mutex.new
    @server.mount("/flaky") do |_, res|
      n = mu.synchronize { attempts += 1 }
      if n < 3
        res.status = 503; res.body = "later"
      else
        res.status = 200; res.body = "<h1>ok</h1>"
      end
    end
    r = Scrapetor::Fetcher.get(@server.url("/flaky"),
                                retry: 4, backoff: 0.01, max_backoff: 0.05)
    assert_equal 200, r[:status]
    assert_equal 3, attempts
  end

  def test_retry_gives_up_and_returns_last_response
    @server.mount("/no") { |_, res| res.status = 503; res.body = "down" }
    r = Scrapetor::Fetcher.get(@server.url("/no"),
                                retry: 2, backoff: 0.01, max_backoff: 0.02)
    assert_equal 503, r[:status]
  end

  def test_etag_cache_round_trip
    @cache_dir = "/tmp/scrap_test_cache_#{Process.pid}_#{rand(1000)}"
    FileUtils.mkdir_p(@cache_dir)
    @server.mount("/p") do |req, res|
      etag = '"v1"'
      if req["If-None-Match"] == etag
        res.status = 304
      else
        res.status = 200
        res["ETag"] = etag
        res["Content-Type"] = "text/html; charset=utf-8"
        res.body = "<h1>cached</h1>"
      end
    end
    cold = Scrapetor::Fetcher.get(@server.url("/p"), cache_dir: @cache_dir)
    warm = Scrapetor::Fetcher.get(@server.url("/p"), cache_dir: @cache_dir)
    assert_equal 200, cold[:status]
    assert_equal 200, warm[:status]
    assert_equal cold[:body], warm[:body]
    assert_equal "hit", warm[:headers]["x-scrapetor-cache"]
  end

  def test_parallel_get_returns_results_in_input_order
    %w[a b c d].each do |path|
      @server.mount("/#{path}") do |_, res|
        res.status = 200; res.body = "<h1>#{path}</h1>"
      end
    end
    urls = %w[a b c d].map { |p| @server.url("/#{p}") }
    rs = Scrapetor::Fetcher.parallel_get(urls, threads: 4)
    assert_equal 4, rs.size
    assert_equal %w[a b c d], rs.map { |r| r[:body][/>([a-z])</, 1] }
  end

  def test_parallel_fetch_returns_documents
    @server.mount("/p") do |_, res|
      res.status = 200; res["Content-Type"] = "text/html; charset=utf-8"
      res.body = "<html><body><h1>doc</h1></body></html>"
    end
    docs = Scrapetor::Fetcher.parallel_fetch([@server.url("/p")] * 3, threads: 3)
    assert_equal 3, docs.size
    docs.each { |d| assert_equal "doc", d.at_css("h1").text }
  end

  def test_multi_get_completes_all_urls
    %w[a b c d e].each do |p|
      @server.mount("/#{p}") { |_, res| res.status = 200; res.body = p }
    end
    urls = %w[a b c d e].map { |p| @server.url("/#{p}") }
    rs = Scrapetor::Fetcher.multi_get(urls)
    assert_equal 5, rs.size
    rs.each { |r| assert_equal 200, r[:status] }
  end

  def test_multi_each_yields_in_completion_order
    # Two endpoints; the "slow" one delayed so the "fast" yields first.
    @server.mount("/slow") { |_, res| sleep 0.15; res.status = 200; res.body = "slow" }
    @server.mount("/fast") { |_, res| sleep 0.02; res.status = 200; res.body = "fast" }
    order = []
    urls = [@server.url("/slow"), @server.url("/fast")]
    Scrapetor::Fetcher.multi_each(urls) { |r| order << r[:body] }
    assert_equal %w[fast slow], order
  end

  def test_revalidate_classifies_fresh_changed_missing
    @cache_dir = "/tmp/scrap_test_rev_#{Process.pid}_#{rand(1000)}"
    FileUtils.mkdir_p(@cache_dir)
    state = { "a" => 1, "b" => 1 }
    %w[a b].each do |p|
      @server.mount("/#{p}") do |req, res|
        etag = "\"v#{state[p]}\""
        if req["If-None-Match"] == etag
          res.status = 304
        else
          res.status = 200; res["ETag"] = etag
          res["Content-Type"] = "text/html; charset=utf-8"
          res.body = "<h1>#{p}#{state[p]}</h1>"
        end
      end
    end
    @server.mount("/c") { |_, res| res.status = 404; res.body = "gone" }
    %w[a b].each { |p| Scrapetor::Fetcher.get(@server.url("/#{p}"), cache_dir: @cache_dir) }
    state["b"] = 2
    urls = %w[a b c].map { |p| @server.url("/#{p}") }
    out = Scrapetor::Fetcher.revalidate(urls, cache_dir: @cache_dir)
    assert_equal :fresh,   out[urls[0]]
    assert_equal :changed, out[urls[1]]
    assert_equal :missing, out[urls[2]]
  end

  def test_custom_user_agent_header
    seen_ua = nil
    @server.mount("/u") do |req, res|
      seen_ua = req["user-agent"]
      res.status = 200; res.body = "ok"
    end
    Scrapetor::Fetcher.get(@server.url("/u"), user_agent: "MyBot/9000")
    assert_equal "MyBot/9000", seen_ua
  end
end
