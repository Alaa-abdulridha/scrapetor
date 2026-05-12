# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

class TestSAX < Minitest::Test
  class Recorder < Scrapetor::SAX::Document
    attr_reader :events

    def initialize
      @events = []
    end

    def start_document;          @events << [:doc_start]; end
    def end_document;            @events << [:doc_end];   end
    def start_element(n, a);     @events << [:start, n, a]; end
    def end_element(n);          @events << [:end, n];      end
    def characters(t);           @events << [:text, t];     end
    def comment(t);              @events << [:comment, t];  end
    def doctype(n);              @events << [:doctype, n];  end
  end

  def parse(html)
    r = Recorder.new
    Scrapetor::SAX::Parser.new(r).parse(html)
    r.events
  end

  def test_simple_document
    events = parse("<p>hi</p>")
    assert_equal [:doc_start], events.first
    assert_equal [:doc_end],   events.last
    starts = events.select { |e| e[0] == :start }
    assert_equal 1, starts.size
    assert_equal "p", starts[0][1]
  end

  def test_attributes_captured
    events = parse('<a href="/x" data-id="42">link</a>')
    s = events.find { |e| e[0] == :start }
    assert_equal "a", s[1]
    assert_equal "/x", s[2]["href"]
    assert_equal "42", s[2]["data-id"]
  end

  def test_text_event
    events = parse("<p>hello world</p>")
    text = events.find { |e| e[0] == :text }
    assert_equal "hello world", text[1]
  end

  def test_nested_structure
    events = parse("<div><span>x</span><span>y</span></div>")
    names = events.select { |e| e[0] == :start }.map { |e| e[1] }
    assert_equal %w[div span span], names
    ends = events.select { |e| e[0] == :end }.map { |e| e[1] }
    assert_equal %w[span span div], ends
  end

  def test_void_emits_immediate_end
    events = parse("<div><img src=/a.png><br></div>")
    starts = events.select { |e| e[0] == :start }.map { |e| e[1] }
    ends   = events.select { |e| e[0] == :end   }.map { |e| e[1] }
    assert_equal %w[div img br], starts
    # void elements get a synthetic end
    assert_includes ends, "img"
    assert_includes ends, "br"
  end

  def test_self_closing
    events = parse("<div><img/></div>")
    ends = events.select { |e| e[0] == :end }.map { |e| e[1] }
    assert_equal %w[img div], ends
  end

  def test_comment_event
    events = parse("<div><!-- note --></div>")
    c = events.find { |e| e[0] == :comment }
    assert_equal " note ", c[1]
  end

  def test_doctype_event
    events = parse("<!DOCTYPE html><html><body></body></html>")
    dt = events.find { |e| e[0] == :doctype }
    assert_equal "html", dt[1]
  end

  def test_script_raw_content
    events = parse("<div><script>var x = '<not-a-tag>';</script><p>after</p></div>")
    # Script content should appear as one text event between script start and end.
    in_script = false
    script_text = +""
    events.each do |e|
      if e[0] == :start && e[1] == "script"
        in_script = true
      elsif e[0] == :end && e[1] == "script"
        in_script = false
      elsif e[0] == :text && in_script
        script_text << e[1]
      end
    end
    assert_includes script_text, "<not-a-tag>"
    # The <p>after</p> after the script must be parsed normally
    p_starts = events.select { |e| e[0] == :start && e[1] == "p" }
    assert_equal 1, p_starts.size
  end

  def test_unquoted_attribute
    events = parse('<a href=/x class=lk>x</a>')
    s = events.find { |e| e[0] == :start }
    assert_equal "/x", s[2]["href"]
    assert_equal "lk", s[2]["class"]
  end

  def test_tokenizer_enumerator
    enum = Scrapetor::SAX::Tokenizer.new("<a>x</a>").each_event
    types = enum.to_a.map(&:first)
    assert_includes types, :start
    assert_includes types, :end
    assert_includes types, :text
  end

  def test_empty_input
    events = parse("")
    assert_equal [[:doc_start], [:doc_end]], events
  end
end
