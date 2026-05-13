# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
$LOAD_PATH.unshift File.expand_path("./support", __dir__)
require "minitest/autorun"
require "fileutils"
require "scrapetor"

begin
  require "webrick"
  require "local_server"
  TEST_FETCHER_EXTRAS_WEBRICK = true
rescue LoadError
  TEST_FETCHER_EXTRAS_WEBRICK = false
end

# Tests for the mTLS / proxy auth / streaming download / bandwidth cap
# options. mTLS handshake itself isn't tested (would need a server
# configured for client-cert validation); we verify the options
# reach libcurl and don't blow up.
class TestFetcherExtras < Minitest::Test
  def self.runnable_methods
    return [] unless Scrapetor::Fetcher.available?
    return [] unless TEST_FETCHER_EXTRAS_WEBRICK
    super
  end

  def setup
    @server = Scrapetor::TestSupport::LocalServer.new
  end

  def teardown
    @server.stop
  end

  def test_download_to_streams_body_to_file_and_keeps_body_empty
    big = "X" * (256 * 1024)  # 256 KB; large enough that any
                              # buffering would show up in :body.
    @server.mount("/big") do |_, res|
      res.body = big
      res["Content-Type"] = "application/octet-stream"
      res.status = 200
    end
    out = "/tmp/scrap_dl_#{Process.pid}_#{rand(1000)}.bin"
    begin
      r = Scrapetor::Fetcher.get(@server.url("/big"), download_to: out)
      assert_equal 200, r[:status]
      assert_equal "", r[:body], "body should be empty when download_to is set"
      assert_equal out, r[:downloaded_to]
      assert File.exist?(out), "expected #{out} to exist"
      assert_equal big.bytesize, File.size(out)
    ensure
      File.delete(out) if File.exist?(out)
    end
  end

  def test_download_to_creates_parent_directory_not_implied
    out = "/tmp/scrap_dl_no_parent_#{Process.pid}/x.bin"
    @server.mount("/x") { |_, res| res.body = "ok"; res.status = 200 }
    # We don't pre-create the directory; libcurl's fopen should fail.
    err = assert_raises(IOError) do
      Scrapetor::Fetcher.get(@server.url("/x"), download_to: out)
    end
    assert_match(/cannot open download_to/, err.message)
  end

  def test_max_recv_bps_option_is_accepted
    @server.mount("/x") { |_, res| res.body = "ok"; res.status = 200 }
    r = Scrapetor::Fetcher.get(@server.url("/x"), max_recv_bps: 1_000_000)
    assert_equal 200, r[:status]  # smoke: caps don't blow up the option-parse path
  end

  def test_max_send_bps_option_is_accepted
    @server.mount("/x") { |req, res|
      res.body = req.body.to_s; res.status = 200
    }
    r = Scrapetor::Fetcher.post(@server.url("/x"),
                                 body: "y" * 1024, max_send_bps: 5_000_000)
    assert_equal 200, r[:status]
  end

  def test_unknown_proxy_type_falls_through_to_http_default
    # We don't have a real proxy in CI, so this just verifies the
    # option parses through without raising — proxy_type = something
    # libcurl understands as one of its enum aliases.
    @server.mount("/x") { |_, res| res.body = "ok"; res.status = 200 }
    r = Scrapetor::Fetcher.get(@server.url("/x"), proxy_type: "http")
    assert_equal 200, r[:status]
  end

  def test_ssl_options_accepted_without_real_handshake
    # The server is plaintext HTTP so the SSL options never actually
    # engage. The test verifies they pass through option parsing.
    @server.mount("/x") { |_, res| res.body = "ok"; res.status = 200 }
    r = Scrapetor::Fetcher.get(@server.url("/x"),
                                ssl_cert: "/nonexistent.pem",
                                ssl_cert_type: "PEM")
    assert_equal 200, r[:status]
  end

  def test_ca_path_option_is_accepted
    @server.mount("/x") { |_, res| res.body = "ok"; res.status = 200 }
    r = Scrapetor::Fetcher.get(@server.url("/x"),
                                ca_path: "/etc/ssl/cert.pem")
    assert_equal 200, r[:status]
  end

  def test_download_to_with_redirect_writes_final_body
    @server.mount("/r") do |_, res|
      res.status = 302; res["Location"] = "/final"; res.body = "redirect"
    end
    @server.mount("/final") do |_, res|
      res.status = 200; res.body = "FINAL-BODY-CONTENT"
    end
    out = "/tmp/scrap_dl_redir_#{Process.pid}.bin"
    begin
      r = Scrapetor::Fetcher.get(@server.url("/r"), download_to: out)
      assert_equal 200, r[:status]
      assert_equal "FINAL-BODY-CONTENT", File.read(out)
      assert_match(%r{/final$}, r[:final_url])
    ensure
      File.delete(out) if File.exist?(out)
    end
  end
end
