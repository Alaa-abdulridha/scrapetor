# frozen_string_literal: true

require "tempfile"
require "uri"
require "thread"

module Scrapetor
  # Stateful HTTP session. Wraps Scrapetor::Fetcher with:
  #   - persistent cookie jar (libcurl COOKIEJAR/COOKIEFILE)
  #   - default headers merged into every request
  #   - basic / bearer auth applied automatically
  #   - per-host rate limiting (polite throttle)
  #   - default retry/backoff
  #   - auto charset transcoding of HTML bodies to UTF-8
  #
  #   session = Scrapetor::Session.new(
  #     cookies:     true,          # ephemeral tempfile jar
  #     user_agent:  "MyBot/1.0",
  #     rate_limit:  0.5,           # min seconds between same-host requests
  #     retry:       3,
  #     headers:     { "Accept-Language" => "en-US" },
  #   )
  #   doc = session.fetch("https://example.com/login")
  #   session.post("https://example.com/login", form: { user: "x", pass: "y" })
  #   doc = session.fetch("https://example.com/dashboard")
  #
  # Cookies set during the login persist for the dashboard call.
  class Session
    DEFAULT_HEADERS = {
      "Accept"          => "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
      "Accept-Language" => "en-US,en;q=0.5",
    }.freeze

    attr_reader :cookie_jar_path

    def initialize(cookies: true,
                   user_agent: nil,
                   headers: {},
                   basic_auth: nil,
                   bearer_token: nil,
                   proxy: nil,
                   ca_path: nil,
                   rate_limit: nil,
                   retry: 0,
                   backoff: 0.3,
                   max_backoff: 10.0,
                   timeout_ms: 30_000,
                   follow_redirects: true,
                   insecure: false,
                   transcode_charset: true)
      Scrapetor::Fetcher.ensure_available!
      @cookie_jar_path =
        case cookies
        when String then cookies
        when true   then ephemeral_jar_path
        when false, nil then nil
        else raise ArgumentError, "cookies: must be String/true/false"
        end
      @defaults = {
        user_agent: user_agent || Scrapetor::Fetcher::DEFAULT_USER_AGENT,
        headers: DEFAULT_HEADERS.merge(headers),
        basic_auth: basic_auth,
        bearer_token: bearer_token,
        proxy: proxy,
        ca_path: ca_path,
        retry: binding.local_variable_get(:retry),
        backoff: backoff,
        max_backoff: max_backoff,
        timeout_ms: timeout_ms,
        follow_redirects: follow_redirects,
        insecure: insecure,
      }.compact
      @defaults[:transcode_utf8] = transcode_charset
      @defaults[:rate_limit_ms] = (rate_limit * 1000).to_i if rate_limit
    end

    %w[get post put patch delete head].each do |verb|
      define_method(verb) do |url, **opts|
        merged = merge_opts(opts)
        Scrapetor::Fetcher.public_send(verb, url, **merged)
      end
    end

    # GET + parse to a Document.
    def fetch(url, **opts)
      resp = get(url, **opts)
      raise Scrapetor::Fetcher::FetchError.new(
        "Session.fetch #{url} -> HTTP #{resp[:status]}",
        status: resp[:status], response: resp
      ) if resp[:status] < 200 || resp[:status] >= 400
      Scrapetor.parse(resp[:body], base_url: resp[:final_url])
    end

    # parallel_get respects the session's defaults (cookies, headers,
    # auth, per-host rate limit). The native batch honours
    # rate_limit_ms per-host via a shared C-side throttle table, so N
    # parallel workers hitting one host all queue at that gate while
    # different hosts run concurrently.
    def parallel_get(urls, **opts)
      merged = merge_opts(opts)
      Scrapetor::Fetcher.parallel_get(urls, **merged)
    end

    def close
      File.delete(@cookie_jar_path) if @cookie_jar_path && File.exist?(@cookie_jar_path) && @ephemeral
    rescue StandardError
      # tempfile may have already been GC'd; ignore
    end

    private

    def ephemeral_jar_path
      @ephemeral = true
      f = Tempfile.new(["scrapetor_jar", ".txt"])
      f.close
      path = f.path
      ObjectSpace.define_finalizer(self, self.class.send(:make_jar_finalizer, path))
      path
    end

    def self.make_jar_finalizer(path)
      proc { File.delete(path) if File.exist?(path) rescue nil }
    end

    def merge_opts(opts)
      m = @defaults.merge(opts) do |_, ours, theirs|
        if ours.is_a?(Hash) && theirs.is_a?(Hash)
          ours.merge(theirs)
        else
          theirs.nil? ? ours : theirs
        end
      end
      if @cookie_jar_path
        m[:cookiejar]  ||= @cookie_jar_path
        m[:cookiefile] ||= @cookie_jar_path
      end
      m
    end

  end
end
