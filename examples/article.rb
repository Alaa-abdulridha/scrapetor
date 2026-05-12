# frozen_string_literal: true
#
# Example: extract a news article (top-level fields + structured data).
#
#   ruby -Ilib examples/article.rb path/to/article.html

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "scrapetor"
require "json"

path = ARGV[0] || File.expand_path("../benchmark/fixtures/article.html", __dir__)
doc  = Scrapetor.parse_file(path)

schema = Scrapetor.schema do
  field :title,     from: "h1.headline", clean: true, required: true
  field :author,    from: ".byline .author", clean: true
  field :date,      from: "time.date", attr: :datetime, type: :date
  field :body,      from: "section.body", type: :html
  field :tags,      from: "a[rel='tag']", multi: true
end

article  = doc.extract(schema)
metadata = {
  page_type:    doc.page_type,
  opengraph:    doc.opengraph,
  twitter:      doc.twitter_card,
  schema_org:   doc.schema_org(type: "NewsArticle")
}

puts JSON.pretty_generate(article: article, metadata: metadata)
