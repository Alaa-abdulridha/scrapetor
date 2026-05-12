# frozen_string_literal: true

module Scrapetor
  # Pure-Ruby HTML construction DSL. No external dependency.
  #
  # Two usage patterns:
  #
  #   # 1. Block with explicit receiver:
  #   html = Scrapetor::Builder.build do |b|
  #     b.html do
  #       b.head { b.title "My Page" }
  #       b.body do
  #         b.h1 "Hello", class: "hdr"
  #         b.p "world", id: "lead"
  #         b.a("More", href: "/x")
  #       end
  #     end
  #   end
  #
  #   # 2. Direct instance:
  #   b = Scrapetor::Builder.new
  #   b.div(class: "card") { b.h1 "Title" }
  #   b.to_html
  class Builder
    VOID = %w[
      area base br col embed hr img input link meta source track wbr
    ].freeze
    private_constant :VOID

    def self.build(&block)
      new(&block).to_html
    end

    def initialize(&block)
      @stack = []
      @root  = []
      if block
        if block.arity == 1
          block.call(self)
        else
          instance_eval(&block)
        end
      end
    end

    # Inject a raw text node at the current position.
    def text(s)
      append(s.to_s)
      self
    end

    # Inject pre-escaped raw HTML.
    def raw(s)
      append(RawHTML.new(s.to_s))
      self
    end

    # Inject an HTML comment.
    def comment(s)
      append(Comment.new(s.to_s))
      self
    end

    # Inject a doctype.
    def doctype(name = "html")
      append(Doctype.new(name.to_s))
      self
    end

    # Method-missing dispatch: any unknown method becomes a tag.
    #
    #   b.div("hi", class: "card") { b.span "x" }
    #     ->  <div class="card">hi<span>x</span></div>
    def method_missing(name, *args, &block)
      content = nil
      attrs   = {}
      args.each do |a|
        case a
        when Hash   then attrs = attrs.merge(a)
        when String then content ||= a
        else             content ||= a.to_s
        end
      end
      element = Element.new(name.to_s, attrs, [])
      append(element)
      @stack.push(element)
      element.children << content unless content.nil?
      if block
        if block.arity == 1
          block.call(self)
        else
          instance_eval(&block)
        end
      end
      @stack.pop
      self
    end

    def respond_to_missing?(_name, _include_private = false)
      true
    end

    def to_html
      @root.map { |n| serialize(n) }.join
    end
    alias to_s to_html

    private

    def append(node)
      if @stack.empty?
        @root << node
      else
        @stack.last.children << node
      end
    end

    def serialize(node)
      case node
      when String
        escape_text(node)
      when RawHTML
        node.body
      when Comment
        "<!--#{node.body}-->"
      when Doctype
        "<!DOCTYPE #{node.body}>"
      when Element
        attr_str = node.attrs.map { |k, v| %( #{k}="#{escape_attr(v)}") }.join
        if VOID.include?(node.name) && node.children.empty?
          "<#{node.name}#{attr_str}>"
        else
          inner = node.children.map { |c| serialize(c) }.join
          "<#{node.name}#{attr_str}>#{inner}</#{node.name}>"
        end
      end
    end

    def escape_text(s)
      s.to_s.gsub(/[&<>]/, "&" => "&amp;", "<" => "&lt;", ">" => "&gt;")
    end

    def escape_attr(s)
      s.to_s.gsub(/[&<>"']/,
                  "&" => "&amp;",
                  "<" => "&lt;",
                  ">" => "&gt;",
                  '"' => "&quot;",
                  "'" => "&#39;")
    end

    Element = Struct.new(:name, :attrs, :children)
    RawHTML = Struct.new(:body)
    Comment = Struct.new(:body)
    Doctype = Struct.new(:body)
    private_constant :Element, :RawHTML, :Comment, :Doctype
  end
end
