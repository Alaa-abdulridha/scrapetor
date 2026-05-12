# frozen_string_literal: true

require "uri"

module Scrapetor
  module URL
    ABSOLUTE = %r{\A[a-zA-Z][\w+.\-]*://}.freeze

    def self.absolute(href, base = nil)
      return nil if href.nil?
      h = href.to_s
      return h if h.match?(ABSOLUTE)
      return h if base.nil?
      begin
        URI.join(base.to_s, h).to_s
      rescue URI::InvalidURIError, ArgumentError
        h
      end
    end
  end
end
