# frozen_string_literal: true

module Scrapetor
  # Native HTTP/2-capable fetch layer. Wraps the libcurl-backed
  # Scrapetor::Native::Http module. Distinct from Scrapetor::HTTP /
  # Scrapetor.fetch, which is the Net::HTTP-based fallback used by
  # tests, the CLI, and environments without libcurl.
  #
  # Capabilities depend on the libcurl your gem links against. Inspect
  # Scrapetor::Fetcher.features to see what's actually wired up
  # (HTTP/2 / brotli / zstd / libz). HTTP/2 + gzip is the typical
  # baseline on macOS; brotli/zstd need a libcurl rebuilt with them.
  #
  # The connection cache lives on a per-OS-thread libcurl easy handle —
  # repeated fetches to the same host inside a single thread reuse the
  # TLS session and HTTP/2 stream. The fetch itself drops the GVL, so
  # background Ruby threads keep running during the round-trip.
  #
  #   resp = Scrapetor::Fetcher.get("https://api.example.com/items")
  #   resp[:status]        # => 200
  #   resp[:http_version]  # => "2"
  #   resp[:headers]       # => {"content-type" => "application/json", ...}
  #   resp[:body]          # => "..."
  #
  #   doc = Scrapetor::Fetcher.fetch("https://example.com/")
  #   # => Scrapetor::Document parsed from the response body, base_url
  #   #    set to the final URL after redirects.
  module Fetcher
    class NotAvailableError < StandardError; end
    class FetchError < StandardError
      attr_reader :status, :response
      def initialize(msg, status: nil, response: nil)
        super(msg)
        @status = status
        @response = response
      end
    end

    DEFAULT_USER_AGENT = "scrapetor/#{Scrapetor::VERSION} (libcurl)"

    def self.available?
      defined?(Scrapetor::Native::Http::AVAILABLE) &&
        Scrapetor::Native::Http::AVAILABLE
    end

    def self.features
      ensure_available!
      Scrapetor::Native::Http.features
    end

    def self.get(url, **opts)
      ensure_available!
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      Scrapetor::Native::Http.get(url.to_s, opts)
    end

    # Fetch + parse. Raises FetchError on non-2xx status by default;
    # pass raise_for_status: false to inspect non-2xx responses.
    def self.fetch(url, raise_for_status: true, **opts)
      resp = get(url, **opts)
      if raise_for_status && (resp[:status] < 200 || resp[:status] >= 400)
        raise FetchError.new(
          "Scrapetor::Fetcher.fetch #{url} -> HTTP #{resp[:status]}",
          status: resp[:status], response: resp
        )
      end
      Scrapetor.parse(resp[:body], base_url: resp[:final_url])
    end

    def self.ensure_available!
      return if available?
      raise NotAvailableError,
            "Scrapetor::Fetcher requires libcurl at build time. " \
            "Reinstall after `brew install curl` / `apt-get install libcurl4-openssl-dev`."
    end
  end

  # Top-level shorthand for the libcurl path. Distinct from
  # Scrapetor.fetch (Net::HTTP) so callers can opt-in to HTTP/2 +
  # connection reuse where it's actually available.
  def self.fetch_http2(url, **opts)
    Fetcher.fetch(url, **opts)
  end
end
