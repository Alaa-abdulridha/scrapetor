# frozen_string_literal: true

require "uri"

module Scrapetor
  # robots.txt parser + path-match decider.
  #
  #   r = Scrapetor::Robots.fetch_for("https://example.com")
  #   r.allowed?("https://example.com/private")
  #   r.crawl_delay
  #   r.sitemaps
  #
  # Implements the de-facto Google / RFC 9309 longest-match semantics:
  # the most-specific (longest pattern) Allow/Disallow rule wins.
  # User-agent matching is case-insensitive prefix; '*' is the fallback.
  class Robots
    Rule = Struct.new(:type, :pattern) # type: :allow or :disallow

    attr_reader :sitemaps

    def initialize(body, user_agent: "*")
      @ua = user_agent
      @groups = {}      # ua_pattern (lowercased) => Array<Rule>
      @delays = {}      # ua_pattern => Float
      @sitemaps = []
      parse!(body.to_s)
    end

    def allowed?(url)
      s = url.to_s
      path =
        if s.start_with?("/")
          s
        else
          uri = URI(s)
          (uri.path.empty? ? "/" : uri.path) + (uri.query ? "?#{uri.query}" : "")
        end
      rules = applicable_rules
      return true if rules.empty?
      # Find the longest matching pattern (Google convention; RFC 9309
      # also says the most specific match wins).
      best = nil
      rules.each do |r|
        next unless path_matches?(path, r.pattern)
        if best.nil? || r.pattern.length > best.pattern.length
          best = r
        end
      end
      best.nil? || best.type == :allow
    end

    def disallowed?(url)
      !allowed?(url)
    end

    def crawl_delay
      ua = ua_for(@ua)
      @delays[ua] || @delays["*"]
    end

    def self.fetch_for(origin, user_agent: "*", **opts)
      uri = URI(origin.to_s)
      url = "#{uri.scheme}://#{uri.host}#{uri.port == uri.default_port ? "" : ":#{uri.port}"}/robots.txt"
      resp = Scrapetor::Fetcher.get(url, raise_for_status: false, **opts)
      body = resp[:status] == 200 ? resp[:body] : ""
      new(body, user_agent: user_agent)
    end

    private

    def applicable_rules
      ua = ua_for(@ua)
      @groups[ua] || @groups["*"] || []
    end

    # Pick the most-specific UA group whose name is a case-insensitive
    # prefix of @ua, or '*' as fallback.
    def ua_for(ua)
      ua_lc = ua.to_s.downcase
      best = nil
      @groups.each_key do |key|
        next if key == "*"
        if ua_lc.start_with?(key) && (best.nil? || key.length > best.length)
          best = key
        end
      end
      best || "*"
    end

    # robots.txt allows '*' wildcards and '$' end-anchor inside patterns.
    # Translate to regex once per call; for hot-path callers, cache.
    def path_matches?(path, pattern)
      regex = pattern_cache(pattern)
      regex.match?(path)
    end

    def pattern_cache(pattern)
      @pattern_cache ||= {}
      @pattern_cache[pattern] ||= compile_pattern(pattern)
    end

    def compile_pattern(pattern)
      buf = +"\\A"
      i = 0
      while i < pattern.length
        ch = pattern[i]
        if ch == "*"
          buf << ".*"
        elsif ch == "$" && i == pattern.length - 1
          buf << "\\z"
        else
          buf << Regexp.escape(ch)
        end
        i += 1
      end
      Regexp.new(buf)
    end

    def parse!(body)
      current_uas = []
      buffer = []
      flush = lambda do
        current_uas.each { |u| (@groups[u] ||= []).concat(buffer) }
        buffer = []
      end
      body.each_line do |line|
        line = line.sub(/#.*\z/, "").strip
        next if line.empty?
        key, val = line.split(":", 2)
        next unless val
        key = key.strip.downcase
        val = val.strip
        case key
        when "user-agent"
          flush.call unless buffer.empty?
          if current_uas.empty? || current_uas.last == val.downcase
            current_uas << val.downcase
          else
            current_uas = [val.downcase]
          end
        when "disallow"
          # An empty disallow means "allow all"; skip — empty pattern would match everything.
          buffer << Rule.new(:disallow, val) unless val.empty?
        when "allow"
          buffer << Rule.new(:allow, val) unless val.empty?
        when "crawl-delay"
          d = val.to_f
          current_uas.each { |u| @delays[u] = d } if d > 0
        when "sitemap"
          @sitemaps << val unless val.empty?
        end
      end
      flush.call unless buffer.empty?
    end
  end
end
