# frozen_string_literal: true

module Scrapetor
  module Dom
    # Build a Dom::Document from raw HTML via the SAX tokenizer.
    module Parser
      VOID_TAGS = Scrapetor::Dom::VOID.to_h { |t| [t, true] }.freeze

      def self.parse(html)
        doc = Dom::Document.new
        stack = [doc]
        tokenizer = Scrapetor::SAX::Tokenizer.new(html)
        tokenizer.each_event do |event|
          type, *args = event
          case type
          when :doc_start, :doc_end
            # no-op
          when :doctype
            doc.doctype = args[0]
          when :start
            name, attrs = args
            element = Element.new(name, attrs || {})
            stack.last.add_child(element)
            stack.push(element) unless VOID_TAGS[element.name]
          when :end
            name = args[0]
            # Pop frames until matching close or root.
            idx = stack.rindex { |n| n.is_a?(Element) && n.name == name }
            if idx
              stack.slice!(idx..)
            end
          when :text
            stack.last.add_child(Text.new(args[0]))
          when :comment
            stack.last.add_child(Comment.new(args[0]))
          end
        end
        doc
      end

      # Parse a fragment — return an Array of nodes (no Document wrapper).
      def self.fragment(html)
        wrapper = Element.new("__fragment__")
        stack = [wrapper]
        Scrapetor::SAX::Tokenizer.new(html).each_event do |event|
          type, *args = event
          case type
          when :start
            name, attrs = args
            element = Element.new(name, attrs || {})
            stack.last.add_child(element)
            stack.push(element) unless VOID_TAGS[element.name]
          when :end
            name = args[0]
            idx = stack.rindex { |n| n.is_a?(Element) && n.name == name }
            stack.slice!(idx..) if idx && idx > 0
          when :text
            stack.last.add_child(Text.new(args[0]))
          when :comment
            stack.last.add_child(Comment.new(args[0]))
          end
        end
        nodes = wrapper.children
        nodes.each { |n| n.parent = nil }
        nodes
      end
    end
  end
end
