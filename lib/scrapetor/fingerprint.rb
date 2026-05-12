# frozen_string_literal: true

module Scrapetor
  # Structural fingerprint of a DOM subtree.
  # Phase 1: tag-bigram rolling hash over the top `depth` levels.
  # Phase 2+: tag bigrams + attribute-presence hash + child-shape hash.
  module Fingerprint
    MASK = 0xFFFFFFFFFFFFFFFF

    def self.structural(node, depth: 4)
      backing = node.respond_to?(:backing_node) ? node.backing_node : node
      h = 0
      walk(backing, depth) do |tag|
        h = (h * 1_315_423_911 + tag.hash) & MASK
      end
      h
    end

    def self.walk(nlx, depth, &block)
      return if depth <= 0
      return unless nlx.respond_to?(:children)
      nlx.children.each do |c|
        next unless c.respond_to?(:element?) && c.element?
        block.call(c.name)
        walk(c, depth - 1, &block)
      end
    end
  end
end
