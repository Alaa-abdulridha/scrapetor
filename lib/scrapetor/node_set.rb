# frozen_string_literal: true

module Scrapetor
  class NodeSet
    include Enumerable

    def initialize(doc, backing_nodes)
      @doc = doc
      @nodes = backing_nodes
    end

    def each
      return enum_for(:each) unless block_given?
      @nodes.each { |n| yield Node.new(@doc, n) }
    end

    def first
      n = @nodes.first
      n && Node.new(@doc, n)
    end

    def last
      n = @nodes.last
      n && Node.new(@doc, n)
    end

    def [](index)
      n = @nodes[index]
      n && Node.new(@doc, n)
    end

    def size
      @nodes.size
    end
    alias length size
    alias count size

    def empty?
      @nodes.empty?
    end

    def map
      return enum_for(:map) unless block_given?
      @nodes.map { |n| yield Node.new(@doc, n) }
    end

    def text
      @nodes.map(&:text).join
    end
    alias inner_text text
    alias content    text

    def at(selector)
      first&.at(selector)
    end
    alias at_css at

    def css(selector)
      collected = []
      @nodes.each do |n|
        next unless n.respond_to?(:css)
        n.css(selector).each { |hit| collected << hit }
      end
      NodeSet.new(@doc, collected)
    end
    alias search css

    def to_html
      @nodes.map { |n| n.respond_to?(:to_html) ? n.to_html : n.to_s }.join
    end
    alias inner_html to_html
    alias to_s to_html

    def attr(name)
      first&.attr(name)
    end
    alias attribute attr

    def reverse
      self.class.new(@doc, @nodes.reverse)
    end

    def +(other)
      other_nodes = other.respond_to?(:backing_nodes) ? other.backing_nodes : Array(other)
      self.class.new(@doc, @nodes + other_nodes)
    end

    def to_a
      map { |n| n }
    end

    def backing_nodes
      @nodes
    end
  end
end
