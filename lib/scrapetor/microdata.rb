# frozen_string_literal: true

module Scrapetor
  # Microdata extractor (HTML5 itemscope / itemprop / itemtype).
  #
  # Walks the DOM looking for itemscope elements and emits a nested
  # hash structure of items + properties. The format mirrors what
  # https://schema.org/docs/datamodel.html describes:
  #
  #   {
  #     "type"       => "https://schema.org/Product",  # from itemtype
  #     "id"         => "...",                          # from itemid
  #     "properties" => {
  #       "name"  => "Widget",
  #       "price" => "19.99",
  #       "offer" => { "type" => "https://schema.org/Offer", ... }
  #     }
  #   }
  module Microdata
    def self.extract(doc)
      items = []
      doc.css("[itemscope]").each do |node|
        # Skip nested items — they'll be reached via the parent's properties.
        next if has_itemscope_ancestor?(node)
        items << build_item(node)
      end
      items
    end

    def self.has_itemscope_ancestor?(node)
      ancestor = node.parent
      while ancestor
        return true if ancestor.respond_to?(:[]) && ancestor["itemscope"]
        ancestor = ancestor.respond_to?(:parent) ? ancestor.parent : nil
      end
      false
    end

    def self.build_item(node)
      item = {}
      item["type"] = node["itemtype"] if node["itemtype"]
      item["id"]   = node["itemid"]   if node["itemid"]
      props = {}
      gather_props(node, props)
      item["properties"] = props
      item
    end

    def self.gather_props(scope, props)
      scope.css("[itemprop]").each do |el|
        # Only direct descendants in microdata terms: an itemprop on a
        # descendant of a nested itemscope belongs to the nested item.
        next if descendant_of_nested_itemscope?(el, scope)

        names = (el["itemprop"] || "").split(/\s+/).reject(&:empty?)
        next if names.empty?
        value = property_value(el)
        names.each do |n|
          if props.key?(n)
            props[n] = [props[n]] unless props[n].is_a?(Array)
            props[n] << value
          else
            props[n] = value
          end
        end
      end
    end

    def self.descendant_of_nested_itemscope?(el, scope)
      cur = el.parent
      while cur && cur != scope
        return true if cur.respond_to?(:[]) && cur["itemscope"]
        cur = cur.respond_to?(:parent) ? cur.parent : nil
      end
      false
    end

    def self.property_value(el)
      if el["itemscope"]
        return build_item(el)
      end
      tag = el.respond_to?(:name) ? el.name.to_s.downcase : ""
      case tag
      when "meta"                  then el["content"]
      when "audio", "embed", "iframe", "img", "source", "track", "video"
        el["src"]
      when "a", "area", "link"     then el["href"]
      when "object"                then el["data"]
      when "data"                  then el["value"] || el.text
      when "meter"                 then el["value"] || el.text
      when "time"                  then el["datetime"] || el.text
      else
        text = el.text.to_s
        text.gsub(/\s+/, " ").strip
      end
    end
  end

  # RDFa extractor — minimal implementation covering the typical
  # subset used on the web (property, content, datatype, typeof).
  module RDFa
    def self.extract(doc)
      out = []
      doc.css("[typeof]").each do |node|
        item = {
          "type"       => node["typeof"],
          "about"      => node["about"] || node["resource"],
          "properties" => collect_props(node)
        }
        out << item
      end
      out
    end

    def self.collect_props(scope)
      props = {}
      scope.css("[property]").each do |el|
        names = (el["property"] || "").split(/\s+/).reject(&:empty?)
        value = el["content"] || el.text.to_s.strip
        names.each do |n|
          if props.key?(n)
            props[n] = [props[n]] unless props[n].is_a?(Array)
            props[n] << value
          else
            props[n] = value
          end
        end
      end
      props
    end
  end
end
