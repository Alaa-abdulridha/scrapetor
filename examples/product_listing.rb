# frozen_string_literal: true
#
# Example: extract product cards from an e-commerce listing page.
#
#   ruby -Ilib examples/product_listing.rb path/to/listing.html

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "scrapetor"
require "json"

path = ARGV[0] || File.expand_path("../benchmark/fixtures/ecommerce_listing.html", __dir__)
base = ARGV[1] || "https://example.com/"

schema = Scrapetor.schema do
  field :page_title, from: "h1", clean: true

  repeated ".product-card", as: :products do
    field :sku,    from: ".product-card", attr: "data-sku"
    field :title,  from: ".product-title", clean: true, required: true
    field :price,  from: ".price", type: :money
    field :rating, from: ".rating", type: :float
    field :url,    from: "a.card-link", attr: :href, type: :url, normalize_url: true
    field :image,  from: "img.product-image", attr: :src, type: :url, normalize_url: true
    field :badges, from: ".badge", multi: true
    field :on_sale, from: ".badge.sale", default: false,
                    transform: ->(v) { !v.nil? }
  end
end

doc    = Scrapetor.parse_file(path, base_url: base)
result = doc.extract(schema)

puts JSON.pretty_generate(result)
