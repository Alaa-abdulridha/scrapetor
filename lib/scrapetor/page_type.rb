# frozen_string_literal: true

module Scrapetor
  # Heuristic page-type detection.
  #
  # Returns one of:
  #   :product_page, :product_listing, :article, :search_results,
  #   :forum_thread, :profile, :documentation, :unknown
  #
  # The heuristic prefers strong signals (JSON-LD @type, OpenGraph
  # og:type) and falls back to structural heuristics (repeated card
  # patterns, byline + body, search bar + result list).
  module PageType
    PRODUCT_OG_TYPES = %w[product product.item og:product].freeze
    ARTICLE_OG_TYPES = %w[article news.article].freeze
    PROFILE_OG_TYPES = %w[profile person og:profile].freeze

    def self.detect(doc)
      from_structured_data(doc) ||
        from_opengraph(doc) ||
        from_structure(doc) ||
        :unknown
    end

    # ----- strong signals: JSON-LD -----

    def self.from_structured_data(doc)
      types = doc.json_ld.flat_map { |item| Array(item.is_a?(Hash) ? item["@type"] : nil) }.compact.map(&:to_s)
      return nil if types.empty?
      return :product_listing if types.include?("ItemList") &&
                                 (types.include?("Product") || types.include?("Offer"))
      return :product_page    if types.include?("Product")
      return :article         if (types & %w[NewsArticle Article BlogPosting]).any?
      return :search_results  if types.include?("SearchResultsPage")
      return :profile         if (types & %w[Person ProfilePage]).any?
      return :forum_thread    if types.include?("DiscussionForumPosting")
      return :documentation   if types.include?("TechArticle")
      nil
    end

    # ----- OpenGraph signals -----

    def self.from_opengraph(doc)
      og = doc.opengraph
      t = (og["type"] || "").to_s.downcase
      return :product_page if PRODUCT_OG_TYPES.any? { |x| t.include?(x) }
      return :article      if ARTICLE_OG_TYPES.any? { |x| t.include?(x) }
      return :profile      if PROFILE_OG_TYPES.any? { |x| t.include?(x) }
      nil
    end

    # ----- structural fallback -----

    def self.from_structure(doc)
      # Search results: a search bar + a list of result items
      if doc.css('input[type="search"], form[role="search"], [class*="search-result"]').any?
        return :search_results
      end

      # Repeated cards = listing
      grid_candidates = %w[
        .product-card .product-tile .product-item .listing-item
        [class*="product-grid"] [class*="card"] [class*="tile"]
      ].flat_map { |sel| doc.css(sel).to_a }.uniq
      return :product_listing if grid_candidates.size >= 6

      # Article: <article> with a byline AND a long body
      articles = doc.css("article")
      if articles.any?
        text = articles.first.text.to_s
        word_count = text.scan(/\S+/).size
        has_byline = doc.css(".byline, .author, [rel='author'], [itemprop='author']").any?
        return :article if word_count >= 200 || has_byline
      end

      # Profile: avatar + name + bio
      if doc.css('[class*="avatar"], [class*="profile-header"]').any? &&
         doc.css('[class*="bio"], [class*="about"]').any?
        return :profile
      end

      # Forum thread
      if doc.css('.thread, .topic, [class*="post-message"]').size >= 2
        return :forum_thread
      end

      # Documentation: code blocks + heading hierarchy
      if doc.css("pre code").size >= 3 && doc.css("h1, h2, h3").size >= 3
        return :documentation
      end

      nil
    end
  end
end
