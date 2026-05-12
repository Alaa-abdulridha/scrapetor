# frozen_string_literal: true

# §16 wedge benchmark.
#
# Compares Scrapetor against Nokogiri and Nokolexbor on the workload the
# plan targets: extract 50 product cards (title, price-as-money, absolute
# URL, image URL) from a representative e-commerce listing page.
#
# Numbers reported by this script must trace to this exact file. Run from
# the repo root:
#
#   ruby -Ilib benchmark/parse_extract.rb

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)

require "benchmark/ips"
require "nokogiri"
require "nokolexbor"
require "scrapetor"

FIXTURE = File.expand_path("fixtures/ecommerce_listing.html", __dir__)
unless File.exist?(FIXTURE)
  warn "Fixture missing; generating..."
  load File.expand_path("fixtures/generate.rb", __dir__)
end
HTML = File.read(FIXTURE)
BASE = "https://example.com/"

SCHEMA = Scrapetor.schema do
  repeated ".product-card", as: :products do
    field :title, from: ".product-title", clean: true
    field :price, from: ".price", type: :money
    field :url,   from: "a.card-link", attr: :href, type: :url, normalize_url: true
    field :image, from: "img.product-image", attr: :src, type: :url, normalize_url: true
  end
end

def nokogiri_extract(html)
  doc = Nokogiri::HTML(html)
  doc.css(".product-card").map do |c|
    {
      title: c.at_css(".product-title")&.text&.gsub(/\s+/, " ")&.strip,
      price: c.at_css(".price")&.text&.gsub(/[^\d.]/, "")&.to_f,
      url:   c.at_css("a.card-link") && URI.join(BASE, c.at_css("a.card-link")["href"]).to_s,
      image: c.at_css("img.product-image") && URI.join(BASE, c.at_css("img.product-image")["src"]).to_s
    }
  end
end

def nokolexbor_extract(html)
  doc = Nokolexbor::HTML(html)
  doc.css(".product-card").map do |c|
    {
      title: c.at_css(".product-title")&.text&.gsub(/\s+/, " ")&.strip,
      price: c.at_css(".price")&.text&.gsub(/[^\d.]/, "")&.to_f,
      url:   c.at_css("a.card-link") && URI.join(BASE, c.at_css("a.card-link")["href"]).to_s,
      image: c.at_css("img.product-image") && URI.join(BASE, c.at_css("img.product-image")["src"]).to_s
    }
  end
end

def scrapetor_dom_extract(html)
  doc = Scrapetor.parse(html, base_url: BASE)
  doc.css(".product-card").map do |c|
    {
      title: c.at(".product-title")&.clean_text,
      price: c.at(".price")&.money,
      url:   c.at("a.card-link")&.absolute_url,
      image: c.at("img.product-image")&.absolute_url
    }
  end
end

def scrapetor_schema_extract(html, schema)
  Scrapetor.parse(html, base_url: BASE).extract(schema)
end

def scrapetor_native_extract(html, schema)
  Scrapetor.extract_native(html, schema, base_url: BASE)
end

# Correctness check before benchmarking — make sure all three produce
# equivalent results.
def sanity_check
  n1 = nokogiri_extract(HTML)
  n2 = nokolexbor_extract(HTML)
  s1 = scrapetor_dom_extract(HTML)
  s2 = scrapetor_schema_extract(HTML, SCHEMA)[:products]
  s3 = scrapetor_native_extract(HTML, SCHEMA)[:products]
  abort "nokogiri/scrapetor(dom) count"    unless n1.size == s1.size
  abort "nokogiri/nokolexbor count"        unless n1.size == n2.size
  abort "nokogiri/scrapetor(schema) count" unless n1.size == s2.size
  abort "nokogiri/scrapetor(native) count" unless n1.size == s3.size
  abort "title mismatch" unless n1[0][:title] == s1[0][:title] && n1[0][:title] == s2[0][:title] && n1[0][:title] == s3[0][:title]
  abort "price mismatch" unless (n1[0][:price] - s1[0][:price]).abs < 0.001 && (n1[0][:price] - s3[0][:price]).abs < 0.001
  abort "url mismatch"   unless n1[0][:url] == s1[0][:url] && n1[0][:url] == s3[0][:url]
  puts "Sanity: all five implementations produce equivalent records (#{n1.size} products)."
end

sanity_check

puts
puts "Workload: extract 50 product cards (title, price-as-money, absolute URL, image URL)"
puts "Fixture: #{File.size(FIXTURE)} bytes, #{HTML.scan(/product-card/).size} cards"
puts

Benchmark.ips do |x|
  x.config(time: 5, warmup: 2)

  x.report("nokogiri")           { nokogiri_extract(HTML) }
  x.report("nokolexbor")         { nokolexbor_extract(HTML) }
  x.report("scrapetor (dom)")    { scrapetor_dom_extract(HTML) }
  x.report("scrapetor (schema)") { scrapetor_schema_extract(HTML, SCHEMA) }
  x.report("scrapetor (native)") { scrapetor_native_extract(HTML, SCHEMA) }

  x.compare!
end
