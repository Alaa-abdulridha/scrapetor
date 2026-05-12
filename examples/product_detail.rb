# frozen_string_literal: true
#
# Example: single-product detail page with nested reviews.
# Exercises both the top-level synthetic root group AND a repeated group.

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "scrapetor"
require "json"

path = ARGV[0] || File.expand_path("../benchmark/fixtures/product.html", __dir__)
doc  = Scrapetor.parse_file(path, base_url: "https://example.com/")

schema = Scrapetor.schema do
  field :title,        from: ".product-title", clean: true, required: true
  field :sku,          from: ".sku-value"
  field :price,        from: ".price", type: :money
  field :rating,       from: ".rating", attr: "data-rating", type: :float
  field :review_count, from: ".review-count", type: :integer
  field :hero,         from: "img.hero", attr: :src, type: :url, normalize_url: true
  field :thumbnails,   from: "img.thumb", attr: :src, type: :url, normalize_url: true, multi: true
  field :features,     from: ".features .feature", multi: true

  repeated ".review", as: :reviews do
    field :id,     from: ".review", attr: "data-review-id"
    field :author, from: ".author"
    field :stars,  from: ".stars", attr: "data-stars", type: :float
    field :body,   from: ".body", clean: true
    field :date,   from: ".date", attr: :datetime, type: :date
  end
end

puts JSON.pretty_generate(doc.extract(schema))
