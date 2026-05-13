# frozen_string_literal: true

require "stringio"

module Scrapetor
  # Sitemap.xml ingestion. Handles both <urlset> (URL listings) and
  # <sitemapindex> (nested sitemap references), streaming so a huge
  # sitemap doesn't have to fit in memory at once.
  #
  #   Scrapetor::Sitemap.urls("https://example.com/sitemap.xml") do |url, meta|
  #     puts url, meta[:lastmod], meta[:priority]
  #   end
  #
  # Or, return an array:
  #
  #   Scrapetor::Sitemap.urls("https://example.com/sitemap.xml").to_a
  module Sitemap
    # Stream-iterate every URL in the sitemap. Recurses into
    # <sitemapindex> entries automatically. Yields (url, meta) where
    # meta carries :lastmod / :changefreq / :priority when present.
    def self.urls(source, depth: 0, max_depth: 5, &block)
      return enum_for(:urls, source, depth: depth, max_depth: max_depth) unless block
      raise ArgumentError, "sitemap recursion too deep" if depth > max_depth
      io = open_source(source)
      Scrapetor.stream(io, outer: "url") do |doc|
        loc = doc.at_css("loc")&.text&.strip
        next unless loc && !loc.empty?
        meta = {
          lastmod:    doc.at_css("lastmod")&.text&.strip,
          changefreq: doc.at_css("changefreq")&.text&.strip,
          priority:   doc.at_css("priority")&.text&.strip,
        }
        yield loc, meta
      end
      # If the file was a sitemapindex instead, the <url> stream above
      # found nothing. Re-open and scan for <sitemap><loc>.
      child_io = open_source(source)
      Scrapetor.stream(child_io, outer: "sitemap") do |doc|
        child_loc = doc.at_css("loc")&.text&.strip
        next unless child_loc
        urls(child_loc, depth: depth + 1, max_depth: max_depth, &block)
      end
    end

    def self.open_source(source)
      return source if source.respond_to?(:read)
      return StringIO.new(source) if source.is_a?(String) && !source.start_with?("http")
      resp = Scrapetor::Fetcher.get(source.to_s)
      StringIO.new(resp[:body])
    end
  end
end
