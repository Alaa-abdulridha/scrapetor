# frozen_string_literal: true

require "uri"

module Scrapetor
  # Pagination helper. Walks a page sequence by detecting the "next
  # page" URL from the document — in priority order:
  #
  #   1. <link rel="next" href="..."> in <head>
  #   2. a[rel~="next"] (most common pattern; HTML spec compliant)
  #   3. The configured CSS selector via :next_link
  #
  # Stops when no next link is found, when max_pages is reached, or
  # when the next URL hasn't changed (defensive against malformed
  # next links pointing at self).
  #
  #   Scrapetor::Pagination.each_page("https://example.com/listings") do |doc, url|
  #     doc.css(".product").each { |p| ... }
  #   end
  #
  # Yields (doc, url) for each page in order. When :http is set to a
  # Scrapetor::Fetcher / Session-like object, it's used for fetches;
  # otherwise Scrapetor::Fetcher (HTTP/2 via libcurl) is used by
  # default, with a Net::HTTP fallback if libcurl isn't available.
  module Pagination
    DEFAULT_MAX_PAGES = 50
    DEFAULT_DELAY     = 0.0

    def self.each_page(start_url, max_pages: DEFAULT_MAX_PAGES,
                       delay: DEFAULT_DELAY, http: nil,
                       next_link: nil)
      return enum_for(:each_page, start_url,
                       max_pages: max_pages, delay: delay,
                       http: http, next_link: next_link) unless block_given?

      url = start_url.to_s
      visited = {}
      page_no = 0
      while url && page_no < max_pages
        break if visited[url]
        visited[url] = true
        page_no += 1

        doc = fetch_page(url, http)
        yield doc, url

        nxt = next_page_url(doc, url, next_link)
        sleep delay if delay > 0 && nxt
        url = nxt
      end
      nil
    end

    # Inspect a document and return the next page URL, or nil if
    # this is the last page. Honours <link rel=next> > a[rel=next] >
    # a custom selector via :next_link.
    def self.next_page_url(doc, current_url, custom_selector = nil)
      # 1. <link rel="next">
      if (link = doc.at_css('link[rel~="next"]'))
        href = link["href"] || link[:href]
        return absolutize(href, current_url) if href && !href.empty?
      end
      # 2. a[rel~="next"]
      doc.css('a[rel~="next"]').each do |a|
        href = a["href"] || a[:href]
        next unless href && !href.empty?
        abs = absolutize(href, current_url)
        return abs if abs && abs != current_url
      end
      # 3. Custom selector — first link element under the match.
      if custom_selector
        node = doc.at_css(custom_selector)
        if node
          # Walk up if user gave us a link target like ".next-link"
          # already pointing at an <a>, or treat as the wrapper and
          # grab the first <a> within.
          link_node = node.respond_to?(:name) && node.name.casecmp?("a") ? node : node.at_css("a")
          if link_node
            href = link_node["href"] || link_node[:href]
            return absolutize(href, current_url) if href && !href.empty?
          end
        end
      end
      nil
    end

    def self.fetch_page(url, http)
      if http && http.respond_to?(:fetch)
        http.fetch(url)
      elsif defined?(Scrapetor::Fetcher) && Scrapetor::Fetcher.available?
        Scrapetor::Fetcher.fetch(url)
      else
        Scrapetor.fetch(url)
      end
    end

    def self.absolutize(href, base)
      return nil if href.nil? || href.empty?
      URI.join(base, href).to_s
    rescue URI::InvalidURIError
      nil
    end
  end

  # Top-level shorthand.
  def self.each_page(start_url, **opts, &block)
    Pagination.each_page(start_url, **opts, &block)
  end
end
