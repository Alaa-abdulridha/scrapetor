# frozen_string_literal: true

require "json"

module Scrapetor
  # Extract structured-data signals every SEO/RAG pipeline needs:
  # JSON-LD, OpenGraph, Twitter Cards, Schema.org microdata.
  #
  # These are deterministic and fast — no DOM walk beyond `doc.css(...)`
  # which is delegated to the backing tokenizer.
  module StructuredData
    JSON_LD_SELECTOR = 'script[type="application/ld+json"]'.freeze

    def self.json_ld(doc)
      out = []
      doc.css(JSON_LD_SELECTOR).each do |script|
        body = script.text
        next if body.nil? || body.strip.empty?
        begin
          parsed = JSON.parse(body)
        rescue JSON::ParserError
          next
        end
        if parsed.is_a?(Array)
          out.concat(parsed)
        elsif parsed.is_a?(Hash) && parsed["@graph"].is_a?(Array)
          out.concat(parsed["@graph"])
        else
          out << parsed
        end
      end
      out
    end

    def self.opengraph(doc)
      collect_meta(doc, prefix: "og:")
    end

    def self.twitter_card(doc)
      collect_meta(doc, prefix: "twitter:")
    end

    def self.schema_org(doc, type: nil)
      list = json_ld(doc)
      return list if type.nil?
      target = type.to_s
      list.select do |item|
        next false unless item.is_a?(Hash)
        t = item["@type"]
        case t
        when String then t == target
        when Array  then t.include?(target)
        else false
        end
      end
    end

    def self.collect_meta(doc, prefix:)
      h = {}
      doc.css("meta").each do |meta|
        # OpenGraph uses `property=`; Twitter Cards use `name=`. Some sites
        # do both. Check both.
        key = meta.attr("property") || meta.attr("name")
        next if key.nil?
        next unless key.start_with?(prefix)
        val = meta.attr("content")
        next if val.nil?
        short_key = key[prefix.length..]
        h[short_key] = val if !h.key?(short_key)
      end
      h
    end
  end
end
