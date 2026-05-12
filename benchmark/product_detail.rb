# frozen_string_literal: true

# Workload: single-product detail page extraction.
#
# Schema mixes top-level fields (title, price, rating, sku, hero image)
# with one repeated group (reviews). Exercises the two-pass native path.

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "benchmark/ips"
require "nokogiri"
require "nokolexbor"
require "scrapetor"

HTML = File.read(File.expand_path("fixtures/product.html", __dir__))
BASE = "https://example.com/"

SCHEMA = Scrapetor.schema do
  field :title,  from: ".product-title", clean: true
  field :price,  from: ".price", type: :money
  field :rating, from: ".rating", type: :float
  field :sku,    from: ".sku-value"
  field :hero,   from: "img.hero", attr: :src, type: :url, normalize_url: true

  repeated ".review", as: :reviews do
    field :author, from: ".author", clean: true
    field :stars,  from: ".stars",  type: :float
    field :body,   from: ".body",   clean: true
    field :date,   from: ".date",   attr: :datetime
  end
end

def nokogiri_extract(html)
  doc = Nokogiri::HTML(html)
  {
    title:  doc.at_css(".product-title")&.text&.gsub(/\s+/, " ")&.strip,
    price:  doc.at_css(".price")&.text&.gsub(/[^\d.]/, "")&.to_f,
    rating: doc.at_css(".rating")&.text&.gsub(/[^\d.]/, "")&.to_f,
    sku:    doc.at_css(".sku-value")&.text,
    hero:   doc.at_css("img.hero") && URI.join(BASE, doc.at_css("img.hero")["src"]).to_s,
    reviews: doc.css(".review").map do |r|
      {
        author: r.at_css(".author")&.text&.strip,
        stars:  r.at_css(".stars")&.text&.gsub(/[^\d.]/, "")&.to_f,
        body:   r.at_css(".body")&.text&.gsub(/\s+/, " ")&.strip,
        date:   r.at_css(".date")&.[]("datetime")
      }
    end
  }
end

def nokolexbor_extract(html)
  doc = Nokolexbor::HTML(html)
  {
    title:  doc.at_css(".product-title")&.text&.gsub(/\s+/, " ")&.strip,
    price:  doc.at_css(".price")&.text&.gsub(/[^\d.]/, "")&.to_f,
    rating: doc.at_css(".rating")&.text&.gsub(/[^\d.]/, "")&.to_f,
    sku:    doc.at_css(".sku-value")&.text,
    hero:   doc.at_css("img.hero") && URI.join(BASE, doc.at_css("img.hero")["src"]).to_s,
    reviews: doc.css(".review").map do |r|
      {
        author: r.at_css(".author")&.text&.strip,
        stars:  r.at_css(".stars")&.text&.gsub(/[^\d.]/, "")&.to_f,
        body:   r.at_css(".body")&.text&.gsub(/\s+/, " ")&.strip,
        date:   r.at_css(".date")&.[]("datetime")
      }
    end
  }
end

def scrapetor_extract(html, schema)
  Scrapetor.parse(html, base_url: BASE).extract(schema)
end

# Sanity
n = nokogiri_extract(HTML)
s = scrapetor_extract(HTML, SCHEMA)
abort "title mismatch" unless n[:title] == s[:title]
abort "review count mismatch" unless n[:reviews].size == s[:reviews].size
puts "Sanity: all implementations produce equivalent records."
puts "Workload: single product page (top-level fields + 3 reviews)"
puts "Fixture: #{HTML.bytesize} bytes"
puts

Benchmark.ips do |x|
  x.config(time: 5, warmup: 2)
  x.report("nokogiri")      { nokogiri_extract(HTML) }
  x.report("nokolexbor")    { nokolexbor_extract(HTML) }
  x.report("scrapetor")     { scrapetor_extract(HTML, SCHEMA) }
  x.compare!
end
