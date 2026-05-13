# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "scrapetor"

class TestParallelParse < Minitest::Test
  def fixture_for(seed)
    buf = +"<!doctype html><html><body>"
    50.times do |i|
      buf << "<div class='r' id='r-#{seed}-#{i}'>"
      buf << "<h2>doc#{seed}-row#{i}</h2>"
      buf << "<span class='price'>$#{i}.99</span>"
      buf << "</div>"
    end
    buf << "</body></html>"
    buf
  end

  def test_parallel_parse_returns_documents_in_input_order
    htmls = 6.times.map { |s| fixture_for(s) }
    docs = Scrapetor.parallel_parse(htmls, threads: 3)
    assert_equal 6, docs.size
    docs.each_with_index do |d, i|
      assert_equal "doc#{i}-row0", d.at_css("h2").text
    end
  end

  def test_parallel_parse_handles_single_document
    docs = Scrapetor.parallel_parse([fixture_for(99)])
    assert_equal 1, docs.size
    assert_equal "doc99-row0", docs.first.at_css("h2").text
  end

  def test_parallel_parse_empty_input
    assert_equal [], Scrapetor.parallel_parse([])
  end

  def test_default_thread_count_capped_by_input_size
    # We can't directly observe the chosen thread count, but the call
    # should not hang or raise on 3 docs without an explicit threads:.
    docs = Scrapetor.parallel_parse(3.times.map { |i| fixture_for(i) })
    assert_equal 3, docs.size
  end

  def test_documents_share_no_state_after_parse
    htmls = 4.times.map { |s| fixture_for(s) }
    docs = Scrapetor.parallel_parse(htmls, threads: 4)
    # Each document's content is its own, regardless of parse-order races.
    docs.each_with_index do |d, i|
      titles = d.css("h2").map(&:text)
      assert_equal 50, titles.size
      titles.each { |t| assert_match(/\Adoc#{i}-row/, t) }
    end
  end

  def test_parallel_parse_with_mutations_isolated_per_doc
    htmls = 3.times.map { |s| fixture_for(s) }
    docs = Scrapetor.parallel_parse(htmls)
    # Mutate doc 0 only — its first .r container gets new inner_html.
    docs[0].at_css(".r").inner_html = "<span>changed</span>"
    assert_match(/changed/, docs[0].at_css(".r").text)
    refute_match(/changed/, docs[1].at_css(".r").text)
    refute_match(/changed/, docs[2].at_css(".r").text)
  end
end
