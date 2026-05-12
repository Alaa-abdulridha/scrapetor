# frozen_string_literal: true

module Scrapetor
  # Pure-Ruby SAX-style streaming HTML parser.
  #
  # The hot path for production extraction is the C streaming engine
  # behind `doc.extract`. This module exists for the cases where you
  # genuinely want token-by-token control — debugging, custom incremental
  # processors, conversion to other formats.
  #
  # Usage:
  #
  #   class MyHandler < Scrapetor::SAX::Document
  #     def start_element(name, attrs); puts "<#{name}>"; end
  #     def end_element(name);          puts "</#{name}>"; end
  #     def characters(text);            puts text; end
  #     def comment(text);               puts "<!--#{text}-->"; end
  #     def doctype(name);               puts "<!DOCTYPE #{name}>"; end
  #   end
  #
  #   Scrapetor::SAX::Parser.new(MyHandler.new).parse(html)
  module SAX
    # Subclass to selectively override callbacks. All default to no-ops.
    class Document
      def start_document; end
      def end_document;   end
      def start_element(name, attrs); end
      def end_element(name); end
      def characters(text); end
      def comment(text); end
      def doctype(name); end
      def cdata_block(text); end
      def error(msg); end
      def warning(msg); end
    end

    class Parser
      def initialize(handler)
        @handler = handler
      end

      def parse(html)
        Tokenizer.new(html).each_event do |event|
          type, *args = event
          case type
          when :doc_start   then @handler.start_document
          when :doc_end     then @handler.end_document
          when :start       then @handler.start_element(args[0], args[1])
          when :end         then @handler.end_element(args[0])
          when :text        then @handler.characters(args[0])
          when :comment     then @handler.comment(args[0])
          when :doctype     then @handler.doctype(args[0])
          when :cdata       then @handler.cdata_block(args[0])
          end
        end
        self
      end

      def parse_file(path)
        parse(File.read(path))
      end

      def parse_io(io)
        parse(io.read)
      end
    end

    # Standalone tokenizer — yields events without going through a handler.
    # Useful when you just want an enumerator:
    #
    #   Scrapetor::SAX::Tokenizer.new(html).each_event do |type, *args|
    #     # ...
    #   end
    class Tokenizer
      VOID = %w[
        area base br col embed hr img input link meta source track wbr
      ].freeze
      RAW_TEXT = %w[script style].freeze

      def initialize(html)
        @html = Scrapetor::Encoding.to_utf8(html)
        @pos  = 0
        @len  = @html.bytesize
      end

      def each_event(&block)
        return enum_for(:each_event) unless block_given?
        block.call([:doc_start])
        while @pos < @len
          ch = byte(@pos)
          if ch == 0x3C # '<'
            handle_open(&block)
          else
            handle_text(&block)
          end
        end
        block.call([:doc_end])
        self
      end

      private

      def byte(i)
        @html.getbyte(i)
      end

      def slice(s, e)
        @html.byteslice(s, e - s) || ""
      end

      def handle_text(&block)
        start = @pos
        while @pos < @len && byte(@pos) != 0x3C
          @pos += 1
        end
        text = slice(start, @pos)
        block.call([:text, text]) unless text.empty?
      end

      def handle_open(&block)
        return unless @pos + 1 < @len

        nxt = byte(@pos + 1)

        # Comment
        if nxt == 0x21 && @pos + 3 < @len && byte(@pos + 2) == 0x2D && byte(@pos + 3) == 0x2D
          start = @pos + 4
          e = @html.index("-->", start)
          if e.nil?
            @pos = @len
            return
          end
          block.call([:comment, slice(start, e)])
          @pos = e + 3
          return
        end

        # Doctype or bogus !
        if nxt == 0x21
          gt = @html.index(">", @pos)
          if gt.nil?
            @pos = @len
            return
          end
          decl = slice(@pos + 2, gt)
          if decl =~ /\A\s*DOCTYPE\b\s*([^\s>]+)?/i
            block.call([:doctype, ($1 || "").downcase])
          end
          @pos = gt + 1
          return
        end

        # End tag
        if nxt == 0x2F # '/'
          @pos += 2
          name_start = @pos
          while @pos < @len && name_char?(byte(@pos))
            @pos += 1
          end
          name = slice(name_start, @pos).downcase
          # Skip to '>'
          while @pos < @len && byte(@pos) != 0x3E
            @pos += 1
          end
          @pos += 1 if @pos < @len
          block.call([:end, name]) unless name.empty?
          return
        end

        # Start tag
        if name_start?(nxt)
          @pos += 1
          name_start = @pos
          while @pos < @len && name_char?(byte(@pos))
            @pos += 1
          end
          name = slice(name_start, @pos).downcase
          attrs = parse_attrs
          self_closing = consume_close
          block.call([:start, name, attrs])
          if VOID.include?(name) || self_closing
            block.call([:end, name])
          elsif RAW_TEXT.include?(name)
            # Raw text content until matching </name>
            text_start = @pos
            needle = "</#{name}"
            close_idx = @html.downcase.index(needle, @pos)
            close_idx ||= @len
            block.call([:text, slice(text_start, close_idx)]) if close_idx > text_start
            @pos = close_idx
            # consume </name ... >
            if @pos < @len
              while @pos < @len && byte(@pos) != 0x3E
                @pos += 1
              end
              @pos += 1 if @pos < @len
              block.call([:end, name])
            end
          end
          return
        end

        # Literal '<' followed by non-name — emit as text
        block.call([:text, "<"])
        @pos += 1
      end

      def parse_attrs
        attrs = {}
        while @pos < @len
          skip_ws
          break if @pos >= @len
          ch = byte(@pos)
          break if ch == 0x3E   # '>'
          break if ch == 0x2F   # '/' (self-closing marker)
          # Attribute name
          name_start = @pos
          while @pos < @len
            nc = byte(@pos)
            break if nc == 0x3D || nc == 0x3E || nc == 0x2F || ws?(nc)
            @pos += 1
          end
          aname = slice(name_start, @pos).downcase
          next if aname.empty?
          skip_ws
          value = nil
          if @pos < @len && byte(@pos) == 0x3D
            @pos += 1
            skip_ws
            if @pos < @len
              q = byte(@pos)
              if q == 0x22 || q == 0x27
                @pos += 1
                val_start = @pos
                while @pos < @len && byte(@pos) != q
                  @pos += 1
                end
                value = slice(val_start, @pos)
                @pos += 1 if @pos < @len
              else
                val_start = @pos
                while @pos < @len && !ws?(byte(@pos)) && byte(@pos) != 0x3E
                  @pos += 1
                end
                value = slice(val_start, @pos)
              end
            end
          end
          attrs[aname] = value || ""
        end
        attrs
      end

      def consume_close
        self_closing = false
        if @pos < @len && byte(@pos) == 0x2F
          self_closing = true
          @pos += 1
        end
        while @pos < @len && byte(@pos) != 0x3E
          @pos += 1
        end
        @pos += 1 if @pos < @len
        self_closing
      end

      def skip_ws
        @pos += 1 while @pos < @len && ws?(byte(@pos))
      end

      def name_start?(b)
        (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A) || b == 0x5F
      end

      def name_char?(b)
        (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A) ||
          (b >= 0x30 && b <= 0x39) || b == 0x2D || b == 0x5F || b == 0x3A
      end

      def ws?(b)
        b == 0x20 || b == 0x09 || b == 0x0A || b == 0x0D || b == 0x0C || b == 0x0B
      end
    end
  end
end
