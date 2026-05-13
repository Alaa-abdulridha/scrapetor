# frozen_string_literal: true

module Scrapetor
  class NodeSet
    include Enumerable

    def initialize(doc, backing_nodes)
      @doc = doc
      if backing_nodes.is_a?(Scrapetor::Native::DocumentWrapper::LazyIds)
        @lazy_ids = backing_nodes
        @nodes    = nil
      else
        @nodes = backing_nodes
      end
    end

    def each
      return enum_for(:each) unless block_given?
      if @lazy_ids
        wrap = @lazy_ids.wrapper
        native = @lazy_ids.native
        @lazy_ids.ids.each do |id|
          yield Node.new(@doc, Scrapetor::Native::Element.new(native, id, wrap))
        end
      else
        @nodes.each { |n| yield Node.new(@doc, n) }
      end
    end

    def first
      if @lazy_ids
        id = @lazy_ids.ids.first
        return nil unless id
        Node.new(@doc, Scrapetor::Native::Element.new(@lazy_ids.native, id, @lazy_ids.wrapper))
      else
        n = @nodes.first
        n && Node.new(@doc, n)
      end
    end

    def last
      if @lazy_ids
        id = @lazy_ids.ids.last
        return nil unless id
        Node.new(@doc, Scrapetor::Native::Element.new(@lazy_ids.native, id, @lazy_ids.wrapper))
      else
        n = @nodes.last
        n && Node.new(@doc, n)
      end
    end

    def [](index)
      if @lazy_ids
        id = @lazy_ids.ids[index]
        return nil unless id
        Node.new(@doc, Scrapetor::Native::Element.new(@lazy_ids.native, id, @lazy_ids.wrapper))
      else
        n = @nodes[index]
        n && Node.new(@doc, n)
      end
    end

    def size
      @lazy_ids ? @lazy_ids.ids.size : @nodes.size
    end
    alias length size
    alias count size

    def empty?
      @lazy_ids ? @lazy_ids.ids.empty? : @nodes.empty?
    end

    def map
      return enum_for(:map) unless block_given?
      if @lazy_ids
        wrap = @lazy_ids.wrapper
        native = @lazy_ids.native
        @lazy_ids.ids.map { |id| yield Node.new(@doc, Scrapetor::Native::Element.new(native, id, wrap)) }
      else
        @nodes.map { |n| yield Node.new(@doc, n) }
      end
    end

    def text
      backing_nodes.map(&:text).join
    end
    alias inner_text text
    alias content    text

    def at(selector)
      first&.at(selector)
    end
    alias at_css at

    def css(selector)
      collected = []
      backing_nodes.each do |n|
        next unless n.respond_to?(:css)
        n.css(selector).each { |hit| collected << hit }
      end
      NodeSet.new(@doc, collected)
    end
    alias search css

    def to_html
      backing_nodes.map { |n| n.respond_to?(:to_html) ? n.to_html : n.to_s }.join
    end
    alias inner_html to_html
    alias to_s to_html

    def attr(name)
      first&.attr(name)
    end
    alias attribute attr

    def reverse
      self.class.new(@doc, backing_nodes.reverse)
    end

    def +(other)
      other_nodes = other.respond_to?(:backing_nodes) ? other.backing_nodes : Array(other)
      self.class.new(@doc, backing_nodes + other_nodes)
    end

    def to_a
      map { |n| n }
    end

    def backing_nodes
      return materialize if @lazy_ids
      @nodes
    end

    # Force the lazy-ids path to allocate its Element wrappers. Used by
    # operations that need the original backing nodes (set algebra,
    # +/-/&, removal).
    def materialize
      return @nodes unless @lazy_ids
      @nodes = @lazy_ids.ids.map { |id| Scrapetor::Native::Element.new(@lazy_ids.native, id, @lazy_ids.wrapper) }
      @lazy_ids = nil
      @nodes
    end

    # ----- Bulk mutation passthroughs -----
    #
    # Nokogiri NodeSet exposes a handful of bulk operations that map onto
    # iterating the underlying nodes. We keep parity so callers can do
    # `doc.css('br').remove` etc. without crashing.

    def remove
      # Two-phase. First promote every backing node to its Dom
      # equivalent (so path-based lookup happens against the still-
      # intact tree); then remove. A naive "iterate + remove" works on
      # a mutable Dom but invalidates the position-index paths the
      # Native::Element fallback relies on after the first deletion.
      resolved = backing_nodes.map do |n|
        if n.respond_to?(:promote_to_dom!)
          n.promote_to_dom!
        else
          n
        end
      end
      resolved.each do |target|
        if target.respond_to?(:remove)
          target.remove
        else
          Node.new(@doc, target).remove
        end
      end
      self
    end
    alias unlink remove

    def each_with_index
      return enum_for(:each_with_index) unless block_given?
      backing_nodes.each_with_index { |n, i| yield Node.new(@doc, n), i }
    end

    def select
      return enum_for(:select) unless block_given?
      kept = []
      backing_nodes.each do |n|
        wrapped = Node.new(@doc, n)
        kept << n if yield(wrapped)
      end
      self.class.new(@doc, kept)
    end
    alias filter select

    def reject
      return enum_for(:reject) unless block_given?
      kept = []
      backing_nodes.each do |n|
        wrapped = Node.new(@doc, n)
        kept << n unless yield(wrapped)
      end
      self.class.new(@doc, kept)
    end

    def find_all
      return enum_for(:find_all) unless block_given?
      select { |n| yield(n) }
    end

    def push(node)
      materialize
      @nodes << (node.is_a?(Node) ? node.backing_node : node)
      self
    end
    alias << push

    def pop
      materialize
      n = @nodes.pop
      n && Node.new(@doc, n)
    end

    def shift
      materialize
      n = @nodes.shift
      n && Node.new(@doc, n)
    end

    def index(node)
      target = node.is_a?(Node) ? node.backing_node : node
      backing_nodes.index(target)
    end

    def include?(node)
      target = node.is_a?(Node) ? node.backing_node : node
      backing_nodes.include?(target)
    end

    def -(other)
      drop = other.respond_to?(:backing_nodes) ? other.backing_nodes : Array(other)
      self.class.new(@doc, backing_nodes - drop)
    end

    def &(other)
      keep = other.respond_to?(:backing_nodes) ? other.backing_nodes : Array(other)
      self.class.new(@doc, backing_nodes & keep)
    end
  end
end
