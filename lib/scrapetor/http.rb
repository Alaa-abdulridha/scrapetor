# frozen_string_literal: true

require "net/http"
require "uri"

module Scrapetor
  # Convenience HTTP fetcher built on `Net::HTTP` (Ruby stdlib — no
  # external runtime dep).
  #
  #   doc = Scrapetor.fetch("https://example.com/products")
  #   doc.css(".product").map { |p| p.at(".title").text }
  #
  # Handles 3xx redirects, sets a sensible User-Agent, applies the
  # response's encoding to the parsed document, and uses the request URL
  # as `base_url` for absolute-URL helpers.
  #
  # For production scraping you'll usually want a real HTTP client
  # (HTTPX, Typhoeus, Faraday) with connection pooling, retries, and
  # cookie storage. `Scrapetor.fetch` is intentionally minimal — it's
  # here so simple scripts and the CLI don't need extra deps.
  module HTTP
    DEFAULT_HEADERS = {
      "User-Agent"      => "Scrapetor/#{Scrapetor::VERSION} (+https://scrapetor.org)",
      "Accept"          => "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
      "Accept-Language" => "en-US,en;q=0.5",
      "Accept-Encoding" => "identity"
    }.freeze

    MAX_REDIRECTS = 5

    class FetchError < Scrapetor::Error; end
    class TooManyRedirects < FetchError; end

    def self.get(url, headers: {}, follow_redirects: true, max_redirects: MAX_REDIRECTS, open_timeout: 10, read_timeout: 30)
      uri = URI(url.to_s)
      raise FetchError, "unsupported scheme: #{uri.scheme.inspect}" unless %w[http https].include?(uri.scheme)

      hops = 0
      loop do
        req = Net::HTTP::Get.new(uri.request_uri)
        DEFAULT_HEADERS.each { |k, v| req[k] = v }
        headers.each { |k, v| req[k.to_s] = v.to_s }

        net = Net::HTTP.new(uri.host, uri.port)
        net.use_ssl     = (uri.scheme == "https")
        net.open_timeout = open_timeout
        net.read_timeout = read_timeout

        resp = net.start { |h| h.request(req) }

        case resp
        when Net::HTTPSuccess
          return Response.new(resp, uri)
        when Net::HTTPRedirection
          raise TooManyRedirects, "exceeded #{max_redirects} redirects" if hops >= max_redirects
          raise FetchError, "redirect with no Location header" unless resp["location"]
          uri = URI.join(uri.to_s, resp["location"])
          hops += 1
          next if follow_redirects
          return Response.new(resp, uri)
        else
          raise FetchError, "HTTP #{resp.code} #{resp.message} for #{uri}"
        end
      end
    end

    # Fetch + parse + return a `Scrapetor::Document` whose `base_url` is
    # the final URL after redirects.
    def self.fetch(url, **opts)
      resp = get(url, **opts)
      Scrapetor.parse(resp.body, base_url: resp.final_url.to_s)
    end

    # Fetch + extract.
    def self.fetch_extract(url, schema, **opts)
      resp = get(url, **opts)
      Scrapetor.parse(resp.body, base_url: resp.final_url.to_s).extract(schema)
    end

    class Response
      attr_reader :net_response, :final_url

      def initialize(net_response, final_url)
        @net_response = net_response
        @final_url    = final_url
      end

      def body
        @net_response.body
      end

      def status
        @net_response.code.to_i
      end

      def headers
        @net_response.to_hash
      end

      def [](header_name)
        @net_response[header_name]
      end
    end
  end

  # Module-level shortcut. Most users only want this.
  def self.fetch(url, **opts)
    HTTP.fetch(url, **opts)
  end

  def self.fetch_extract(url, schema, **opts)
    HTTP.fetch_extract(url, schema, **opts)
  end
end
