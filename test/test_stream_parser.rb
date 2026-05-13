# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "stringio"
require "scrapetor"

class TestStreamParser < Minitest::Test
  def fixture(count = 20, ad_count = 5)
    buf = +"<!doctype html><html><body>"
    buf << "<script>var x = '<div class=\"result\">trap</div>';</script>"
    buf << "<!-- inline <div class='result'>trap</div> -->"
    count.times do |i|
      buf << "<div class='result organic' id='r-#{i}'>"
      buf << "<h2>title-#{i}</h2>"
      buf << "<span class='price'>$#{i}.99</span>"
      buf << "<div>nested-#{i}</div>"  # nested same-tag tests depth tracking
      buf << "</div>"
    end
    ad_count.times do |i|
      buf << "<div class='result ad'>ad-#{i}</div>"
    end
    buf << "</body></html>"
    buf
  end

  def test_streams_all_rows
    rows = Scrapetor.stream(StringIO.new(fixture(10, 3)), outer: "div.result").to_a
    assert_equal 13, rows.size
    assert_equal "title-0", rows.first.at_css("h2").text
  end

  def test_class_filter_with_multiple_required_classes
    rows = Scrapetor.stream(StringIO.new(fixture(10, 3)),
                            outer: "div.result.organic").to_a
    assert_equal 10, rows.size
    rows.each { |r| assert_match(/title-/, r.at_css("h2").text) }
  end

  def test_id_filter
    rows = Scrapetor.stream(StringIO.new(fixture(10, 3)),
                            outer: "div#r-3").to_a
    assert_equal 1, rows.size
    assert_equal "title-3", rows.first.at_css("h2").text
  end

  def test_id_plus_class_filter
    rows = Scrapetor.stream(StringIO.new(fixture(10, 3)),
                            outer: "div#r-5.organic").to_a
    assert_equal 1, rows.size
    rows = Scrapetor.stream(StringIO.new(fixture(10, 3)),
                            outer: "div#r-5.ad").to_a
    assert_equal 0, rows.size
  end

  def test_handles_tiny_chunk_size_across_tag_boundaries
    big = fixture(15, 2)
    rows = Scrapetor.stream(StringIO.new(big), outer: "div.result",
                            chunk_size: 47).to_a
    assert_equal 17, rows.size
  end

  def test_handles_one_byte_chunk_size_pathological
    big = fixture(5, 1)
    rows = Scrapetor.stream(StringIO.new(big), outer: "div.result",
                            chunk_size: 1).to_a
    assert_equal 6, rows.size
  end

  def test_script_and_comment_contents_are_skipped
    rows = Scrapetor.stream(StringIO.new(fixture(3, 0)), outer: "div.result").to_a
    # The fixture has <script>var x = '<div class="result">trap</div>'</script>
    # and an HTML comment containing the same fake tag. Neither should
    # be emitted.
    assert_equal 3, rows.size
    rows.each { |r| refute_match(/trap/, r.at_css("h2").text) }
  end

  def test_fields_mode_extracts_with_native_extract
    h = Scrapetor.stream(StringIO.new(fixture(3, 0)),
                         outer: "div.result",
                         fields: { title: "h2::text", price: ".price::text" }).to_a
    assert_equal 3, h.size
    assert_equal "title-0", h.first[:title].to_s.strip
    assert_equal "$0.99",   h.first[:price].to_s.strip
  end

  def test_returns_enumerator_when_no_block
    enum = Scrapetor.stream(StringIO.new(fixture(2, 0)), outer: "div.result")
    assert_kind_of Enumerator, enum
    assert_equal 2, enum.to_a.size
  end

  def test_rejects_bare_class_selector
    assert_raises(ArgumentError) do
      Scrapetor.stream(StringIO.new(""), outer: ".result") { |_| }
    end
  end

  def test_rejects_multi_id_selector
    assert_raises(ArgumentError) do
      Scrapetor.stream(StringIO.new(""), outer: "div#a#b") { |_| }
    end
  end

  def test_self_closing_target_emits_row
    html = "<html><body><img class='r' src='x'/><img class='r' src='y'/></body></html>"
    rows = Scrapetor.stream(StringIO.new(html), outer: "img.r").to_a
    assert_equal 2, rows.size
  end

  def test_string_input_accepted_via_stream
    rows = Scrapetor.stream(fixture(4, 0), outer: "div.result").to_a
    assert_equal 4, rows.size
  end
end
