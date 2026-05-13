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

    # Status codes worth retrying. 408 (timeout), 425 (early), 429
    # (rate-limit), 500–504 (transient upstream). 5xx >= 505 are
    # protocol-level rejections; they don't usually heal on retry.
    DEFAULT_RETRY_STATUSES = [408, 425, 429, 500, 502, 503, 504].freeze

    # Compute the backoff delay for attempt N (1-indexed). Exponential
    # with full jitter — the AWS-style 'random between 0 and 2^n * base'
    # variant — capped at max_backoff.
    def self.backoff_for(attempt, base: 0.3, max: 10.0, retry_after: nil)
      return [retry_after.to_f, max].min if retry_after && retry_after.to_f > 0
      hi = [base * (2.0**(attempt - 1)), max].min
      rand * hi
    end

    def self.retryable_response?(resp, retry_statuses)
      retry_statuses.any? { |s| s == resp[:status] }
    end

    def self.parse_retry_after(headers)
      v = headers && (headers["retry-after"] || headers["Retry-After"])
      return nil unless v
      # Either integer seconds or HTTP-date. We only honour the
      # integer form (the date form is rare and parsing it adds a
      # dependency on Time.httpdate that the caller may not need).
      v.to_s.strip.match?(/\A\d+\z/) ? v.to_i : nil
    end

    def self.available?
      defined?(Scrapetor::Native::Http::AVAILABLE) &&
        Scrapetor::Native::Http::AVAILABLE
    end

    def self.features
      ensure_available!
      Scrapetor::Native::Http.features
    end

    # Single GET with optional retry + exponential backoff.
    #
    #   retry: 0     - try once and return whatever happens (default).
    #   retry: N     - retry up to N times on transient failure.
    #   backoff:     - base backoff in seconds (default 0.3).
    #   max_backoff: - cap on a single sleep (default 10.0).
    #   retry_on:    - statuses to retry (default [408, 425, 429, 500..504]).
    #
    # Network failures (IOError from libcurl: connect refused, DNS,
    # TLS, timeout) are also retried. The wait between attempts honours
    # Retry-After response headers (numeric form) when present and
    # otherwise uses exponential backoff with full jitter.
    def self.get(url, **opts)
      ensure_available!
      retries        = opts.delete(:retry) || 0
      base           = opts.delete(:backoff) || 0.3
      max_backoff    = opts.delete(:max_backoff) || 10.0
      retry_on       = opts.delete(:retry_on) || DEFAULT_RETRY_STATUSES
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      attempt = 0
      last_err = nil
      loop do
        begin
          resp = Scrapetor::Native::Http.get(url.to_s, opts)
          return resp unless retries > attempt && retryable_response?(resp, retry_on)
          ra = parse_retry_after(resp[:headers])
          sleep backoff_for(attempt + 1, base: base, max: max_backoff,
                            retry_after: ra)
        rescue IOError => e
          last_err = e
          raise e unless retries > attempt
          sleep backoff_for(attempt + 1, base: base, max: max_backoff)
        end
        attempt += 1
      end
    rescue IOError
      raise last_err if last_err
      raise
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

    # N concurrent GETs across pthread workers, each with a persistent
    # libcurl handle (per-OS-thread connection cache). The full batch
    # runs under one GVL release — other Ruby threads stay live
    # throughout.
    #
    #   results = Scrapetor::Fetcher.parallel_get(urls, threads: 8,
    #                                              timeout_ms: 5_000)
    #   # results is Array<Hash>; successful entries carry
    #   #   :status, :headers, :body, :final_url, :http_version
    #   # failed entries carry { error: { url:, error: } } only.
    # N concurrent GETs with optional retry. The native batch returns
    # all results in one pass; failed entries (transient status or
    # network error) get a second batch dispatch under retry, with
    # the previous-attempt's wait honoured globally — i.e. one sleep
    # between attempts rather than per-URL — so the pool keeps moving.
    #
    # Per-URL Retry-After headers are read on retryable responses and
    # the maximum of them governs the next inter-attempt sleep, so a
    # rate-limited host pulls the whole pool to its backoff rather
    # than thrashing the rest in parallel.
    def self.parallel_get(urls, **opts)
      ensure_available!
      urls = Array(urls).map(&:to_s)
      return [] if urls.empty?

      retries     = opts.delete(:retry) || 0
      base        = opts.delete(:backoff) || 0.3
      max_backoff = opts.delete(:max_backoff) || 10.0
      retry_on    = opts.delete(:retry_on) || DEFAULT_RETRY_STATUSES
      opts[:user_agent] ||= DEFAULT_USER_AGENT

      results = Array.new(urls.size)
      pending = (0...urls.size).to_a
      attempt = 0
      loop do
        batch = pending.map { |i| urls[i] }
        batch_res = Scrapetor::Native::Http.parallel_fetch(batch, opts)
        next_pending = []
        next_retry_after = nil
        pending.each_with_index do |orig_i, pos|
          r = batch_res[pos]
          if attempt < retries && retry_eligible?(r, retry_on)
            ra = r[:headers] ? parse_retry_after(r[:headers]) : nil
            next_retry_after = ra if ra && (next_retry_after.nil? || ra > next_retry_after)
            next_pending << orig_i
          else
            results[orig_i] = r
          end
        end
        break if next_pending.empty?
        attempt += 1
        sleep backoff_for(attempt, base: base, max: max_backoff,
                          retry_after: next_retry_after)
        pending = next_pending
      end
      results
    end

    def self.retry_eligible?(r, retry_on)
      return true if r[:error]                       # network-level
      r[:status] && retry_on.any? { |s| s == r[:status] }
    end

    # Single-thread curl_multi bulk fetch — one driver thread, one
    # multi handle, N concurrent transfers multiplexed via
    # curl_multi_perform. Complements parallel_get:
    #
    #   parallel_get  - N pthread workers, each running blocking easy.
    #                   Best when each transfer has CPU work after the
    #                   fetch (decode + parse) since the GVL is released
    #                   across the full batch and CPU scales with cores.
    #
    #   multi_get     - one driver thread, N concurrent in-flight.
    #                   Best for I/O-dominated high-fan-out (hundreds of
    #                   URLs across many hosts) where pthread setup
    #                   overhead outweighs the in-flight count.
    #
    # Both share the same global CURLSH so connections / DNS / TLS
    # sessions pool across them.
    def self.multi_get(urls, **opts)
      ensure_available!
      urls = Array(urls).map(&:to_s)
      return [] if urls.empty?
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      Scrapetor::Native::Http.multi_fetch(urls, opts)
    end

    # Bulk-revalidate cached entries. Issues HEAD with
    # If-None-Match / If-Modified-Since for every URL whose cache
    # entry exists; the server's 304 / 200 verdict classifies each:
    #
    #   :fresh    - server said 304; cache still valid.
    #   :changed  - server returned a new 2xx; cache rewritten.
    #   :missing  - server returned 4xx (gone / not found).
    #   :error    - transport failure.
    #
    # Returns a Hash[url => Symbol]. Optimal for crawls of N pages
    # over moderate intervals: HEAD round-trips are cheap and run
    # all-concurrent under curl_multi.
    def self.revalidate(urls, cache_dir:, **opts)
      ensure_available!
      urls = Array(urls).map(&:to_s)
      return {} if urls.empty?
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      results = Scrapetor::Native::Http.multi_fetch(urls,
        opts.merge(method: :head, cache_dir: cache_dir))
      out = {}
      results.each_with_index do |r, i|
        url = urls[i]
        out[url] =
          if r[:error]
            :error
          elsif r[:headers] && r[:headers]["x-scrapetor-cache"] == "hit"
            :fresh
          elsif r[:status] && r[:status] >= 400
            :missing
          else
            :changed
          end
      end
      out
    end

    # multi_get + per-response parse, all under one no-GVL window.
    # Returns Array<Scrapetor::Document | nil>, in input order. Failed
    # entries are nil. Best for high-fan-out crawls where you want
    # parsed Documents back and the I/O outweighs the per-page
    # CPU cost.
    def self.multi_fetch(urls, **opts)
      urls = Array(urls).map(&:to_s)
      return [] if urls.empty?
      ensure_available!
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      results = Scrapetor::Native::Http.multi_fetch(urls, opts.merge(parse: true))
      results.map do |r|
        next nil if r[:error]
        native = r[:document]
        next Scrapetor.parse(r[:body], base_url: r[:final_url]) unless native
        Scrapetor::Document.new("", base_url: r[:final_url], native: native)
      end
    end

    # Method shorthands. Each is just a `.get` invocation with the
    # corresponding method, plus the body sugar that POST/PUT/PATCH
    # almost always need.
    def self.post(url, body: nil, form: nil, json: nil, multipart: nil, **opts)
      if multipart
        opts[:multipart] = multipart
        get(url, **opts.merge(method: :post))
      else
        body, opts = build_body(body, form, json, opts)
        get(url, **opts.merge(method: :post, body: body))
      end
    end

    # Convenience constructors for multipart values.
    def self.upload_file(path, filename: nil, content_type: nil)
      h = { path: path.to_s }
      h[:filename] = filename if filename
      h[:content_type] = content_type if content_type
      h
    end

    def self.upload_bytes(bytes, filename:, content_type: "application/octet-stream")
      { data: bytes, filename: filename, content_type: content_type }
    end

    def self.put(url, body: nil, form: nil, json: nil, **opts)
      body, opts = build_body(body, form, json, opts)
      get(url, **opts.merge(method: :put, body: body))
    end

    def self.patch(url, body: nil, form: nil, json: nil, **opts)
      body, opts = build_body(body, form, json, opts)
      get(url, **opts.merge(method: :patch, body: body))
    end

    def self.delete(url, **opts)
      get(url, **opts.merge(method: :delete))
    end

    def self.head(url, **opts)
      get(url, **opts.merge(method: :head))
    end

    # Build the request body from one of :body / :form / :json.
    # Returns [body_string, opts_with_content_type_header_set].
    def self.build_body(body, form, json, opts)
      headers = (opts[:headers] || {}).dup
      if json
        require "json"
        body = JSON.generate(json)
        headers["Content-Type"] ||= "application/json"
      elsif form
        require "uri"
        body = URI.encode_www_form(form)
        headers["Content-Type"] ||= "application/x-www-form-urlencoded"
      end
      opts[:headers] = headers unless headers.empty?
      [body, opts]
    end

    # Convenience: parallel_get + parse each successful response into
    # a Scrapetor::Document. Failed entries return nil.
    #
    # The parse runs INSIDE the same no-GVL pthread worker that did
    # the fetch (parse: true on the C side), so the whole batch —
    # network + decode + transcode + dom_parse + index build — runs
    # multi-core under a single GVL release. The main thread only
    # wraps already-parsed documents.
    def self.parallel_fetch(urls, **opts)
      urls = Array(urls).map(&:to_s)
      return [] if urls.empty?
      ensure_available!
      opts[:user_agent] ||= DEFAULT_USER_AGENT
      results = Scrapetor::Native::Http.parallel_fetch(urls, opts.merge(parse: true))
      results.map do |r|
        next nil if r[:error]
        native = r[:document]
        next Scrapetor.parse(r[:body], base_url: r[:final_url]) unless native
        Scrapetor::Document.new("", base_url: r[:final_url], native: native)
      end
    end
  end

  # Top-level shorthand for the libcurl path. Distinct from
  # Scrapetor.fetch (Net::HTTP) so callers can opt-in to HTTP/2 +
  # connection reuse where it's actually available.
  def self.fetch_http2(url, **opts)
    Fetcher.fetch(url, **opts)
  end
end
