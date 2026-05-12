# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "net/http"
require "scrapetor"

# Tests for Scrapetor::HTTP. Stub Net::HTTP at the connection level —
# no live server, no network access.
class TestHTTP < Minitest::Test
  HTML_BODY = '<html><body><h1 class="t">From server</h1><a href="/about" class="lk">about</a></body></html>'

  # Real Net::HTTP subclass instances so `case ... when Net::HTTPSuccess`
  # actually matches.
  def self.make_response(klass, body = "", headers = {})
    obj = klass.allocate
    obj.instance_variable_set(:@body, body)
    obj.instance_variable_set(:@read, true)
    obj.instance_variable_set(:@header, headers.transform_keys(&:downcase).transform_values { |v| Array(v) })
    obj
  end

  class FakeConn
    attr_accessor :use_ssl, :open_timeout, :read_timeout

    def initialize(_host, _port); end
    def start; yield self; end
    def request(_req); TestHTTP.responder.call; end
  end

  class << self
    attr_accessor :responder
  end

  def setup
    @orig_new = Net::HTTP.method(:new)
    silence_warnings do
      Net::HTTP.define_singleton_method(:new) { |host, port = nil| FakeConn.new(host, port) }
    end
  end

  def teardown
    orig = @orig_new
    silence_warnings do
      Net::HTTP.define_singleton_method(:new) { |host, port = nil| orig.call(host, port) }
    end
  end

  def silence_warnings
    orig = $VERBOSE
    $VERBOSE = nil
    yield
  ensure
    $VERBOSE = orig
  end

  def success(body = HTML_BODY, headers = {})
    self.class.make_response(Net::HTTPSuccess, body, headers)
  end

  def redirect(location)
    self.class.make_response(Net::HTTPRedirection, "", "location" => location)
  end

  def server_error
    self.class.make_response(Net::HTTPServerError, "boom")
  end

  def test_fetch_returns_document
    TestHTTP.responder = -> { success }
    doc = Scrapetor.fetch("http://example.com/page.html")
    assert_kind_of Scrapetor::Document, doc
    assert_equal "From server", doc.at(".t").text
  end

  def test_fetch_uses_final_url_as_base
    TestHTTP.responder = -> { success }
    doc = Scrapetor.fetch("http://example.com/page.html")
    assert_equal "http://example.com/about", doc.at(".lk").absolute_url
  end

  def test_redirect_followed_by_default
    hops = 0
    TestHTTP.responder = lambda do
      hops += 1
      hops == 1 ? redirect("http://example.com/page.html") : success
    end
    doc = Scrapetor.fetch("http://example.com/r")
    assert_equal "From server", doc.at(".t").text
  end

  def test_redirect_loop_raises
    TestHTTP.responder = -> { redirect("/loop") }
    assert_raises(Scrapetor::HTTP::TooManyRedirects) do
      Scrapetor.fetch("http://example.com/loop", max_redirects: 3)
    end
  end

  def test_5xx_raises
    TestHTTP.responder = -> { server_error }
    assert_raises(Scrapetor::HTTP::FetchError) do
      Scrapetor.fetch("http://example.com/error")
    end
  end

  def test_unsupported_scheme_raises
    assert_raises(Scrapetor::HTTP::FetchError) do
      Scrapetor.fetch("ftp://example.com/")
    end
  end

  def test_fetch_extract
    TestHTTP.responder = -> { success }
    schema = Scrapetor.schema do
      field :h1, from: ".t", clean: true
      field :link, from: ".lk", attr: :href, type: :url, normalize_url: true
    end
    result = Scrapetor.fetch_extract("http://example.com/page.html", schema)
    assert_equal "From server", result[:h1]
    assert_equal "http://example.com/about", result[:link]
  end
end
