# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "minitest/autorun"
require "scrapetor"

class TestBuilder < Minitest::Test
  def test_simple_tag
    html = Scrapetor::Builder.build { |b| b.div "hi" }
    assert_equal "<div>hi</div>", html
  end

  def test_attributes
    html = Scrapetor::Builder.build { |b| b.div "x", class: "card", id: "main" }
    assert_equal '<div class="card" id="main">x</div>', html
  end

  def test_nested
    html = Scrapetor::Builder.build do |b|
      b.html do
        b.head { b.title "Hi" }
        b.body { b.h1 "Hello" }
      end
    end
    assert_equal "<html><head><title>Hi</title></head><body><h1>Hello</h1></body></html>", html
  end

  def test_void_element
    html = Scrapetor::Builder.build { |b| b.img src: "/a.png", alt: "x" }
    assert_equal '<img src="/a.png" alt="x">', html
  end

  def test_text_escaping
    html = Scrapetor::Builder.build { |b| b.p "a & b < c > d" }
    assert_equal "<p>a &amp; b &lt; c &gt; d</p>", html
  end

  def test_attribute_escaping
    html = Scrapetor::Builder.build { |b| b.div "x", title: 'a "b" <c>' }
    assert_includes html, '"a &quot;b&quot; &lt;c&gt;"'
  end

  def test_raw_html
    html = Scrapetor::Builder.build do |b|
      b.div { b.raw "<i>literal</i>" }
    end
    assert_equal "<div><i>literal</i></div>", html
  end

  def test_comment
    html = Scrapetor::Builder.build do |b|
      b.div { b.comment " note " }
    end
    assert_equal "<div><!-- note --></div>", html
  end

  def test_doctype
    html = Scrapetor::Builder.build do |b|
      b.doctype
      b.html { b.body { b.p "hi" } }
    end
    assert_equal "<!DOCTYPE html><html><body><p>hi</p></body></html>", html
  end

  def test_multiple_children
    html = Scrapetor::Builder.build do |b|
      b.ul do
        b.li "A"
        b.li "B"
        b.li "C"
      end
    end
    assert_equal "<ul><li>A</li><li>B</li><li>C</li></ul>", html
  end

  def test_mixed_text_and_elements
    html = Scrapetor::Builder.build do |b|
      b.p "Visit ", { class: "lead" } do
        b.a "the docs", href: "/docs"
        b.text " for details."
      end
    end
    assert html.include?("<a")
    assert html.include?(' for details.')
  end

  def test_explicit_instance
    b = Scrapetor::Builder.new
    b.div(class: "c") { b.span "x" }
    assert_equal '<div class="c"><span>x</span></div>', b.to_html
  end

  def test_no_block_no_content
    html = Scrapetor::Builder.build { |b| b.br }
    assert_equal "<br>", html
  end

  def test_attrs_with_dashes_and_data
    html = Scrapetor::Builder.build do |b|
      b.div "x", "data-sku": "A1", "aria-label": "card"
    end
    assert_includes html, 'data-sku="A1"'
    assert_includes html, 'aria-label="card"'
  end
end
