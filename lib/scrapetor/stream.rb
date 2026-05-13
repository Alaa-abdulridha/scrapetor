# frozen_string_literal: true

require "stringio"

module Scrapetor
  # Streaming parser. Reads HTML incrementally from an IO and yields one
  # complete row at a time. Peak memory stays bounded to roughly
  # max(read_chunk, longest_row_in_bytes) regardless of total document
  # size, so multi-gigabyte fixtures, paginated dumps, and slow socket
  # feeds work without buffering the whole thing.
  #
  # The "row" boundary is byte-scanned in C — no DOM is built for the
  # outer-document context. Once a row is found, its HTML slice is
  # parsed as a fragment through the standard native path so all the
  # normal Document / Element / extract APIs are available.
  #
  #   Scrapetor.stream(io, outer: "div.result") do |doc|
  #     puts doc.at_css(".title")&.text
  #   end
  #
  # With a schema, each row is run through the native extractor and
  # yielded as a Hash:
  #
  #   Scrapetor.stream(io, outer: "li.product", fields: {
  #     title: ".title::text",
  #     price: ".price::text",
  #   }) do |row|
  #     puts row[:title]
  #   end
  #
  # The outer pattern accepts:
  #   - "tag"          (any element of that name)
  #   - "tag.class"    (element with that class token)
  #   - ".class"       — not supported; provide a tag for byte scanning
  class Stream
    DEFAULT_CHUNK = 64 * 1024

    def initialize(io, outer:, fields: nil, chunk_size: DEFAULT_CHUNK)
      tag, id, classes = self.class.parse_outer(outer)
      @native = Scrapetor::Native::Stream.new(tag, id, classes)
      @io = io
      @fields = fields
      @chunk_size = chunk_size
    end

    def each
      return enum_for(:each) unless block_given?
      loop do
        # Pull every row currently available in the buffer.
        while (row_html = @native.next_row)
          yield materialise(row_html)
        end
        break if @native.done?
        chunk = @io.read(@chunk_size)
        if chunk.nil? || chunk.empty?
          @native.set_eof
          # Final drain after EOF — buffer may still have buffered rows.
          while (row_html = @native.next_row)
            yield materialise(row_html)
          end
          break
        else
          @native.feed(chunk)
        end
      end
      self
    end

    # Accepts:
    #   "tag"                 -> [tag, nil, []]
    #   "tag.class"           -> [tag, nil, ["class"]]
    #   "tag.cls1.cls2"       -> [tag, nil, ["cls1", "cls2"]]
    #   "tag#id"              -> [tag, "id", []]
    #   "tag#id.cls1"         -> [tag, "id", ["cls1"]]
    #   "tag.cls#id"          -> [tag, "id", ["cls"]]   (any order after tag)
    def self.parse_outer(outer)
      m = outer.match(/\A([a-zA-Z][\w-]*)((?:[.#][\w-]+)*)\z/)
      raise ArgumentError,
            "Scrapetor.stream outer must be 'tag', 'tag.class', 'tag#id', " \
            "or 'tag#id.cls1.cls2' (got #{outer.inspect})" unless m
      tag = m[1]
      tail = m[2]
      id = nil
      classes = []
      tail.scan(/([.#])([\w-]+)/).each do |sigil, name|
        if sigil == "#"
          raise ArgumentError,
                "Scrapetor.stream outer: only one #id is supported (got #{outer.inspect})" if id
          id = name
        else
          classes << name
        end
      end
      [tag, id, classes]
    end

    private

    def materialise(row_html)
      doc = Scrapetor.parse(row_html)
      return doc unless @fields
      root = doc.css("*").first || doc
      root.extract(@fields)
    end
  end

  def self.stream(source, outer:, fields: nil, chunk_size: Stream::DEFAULT_CHUNK, &block)
    io = source.respond_to?(:read) ? source : StringIO.new(source)
    Stream.new(io, outer: outer, fields: fields, chunk_size: chunk_size).each(&block)
  end
end
