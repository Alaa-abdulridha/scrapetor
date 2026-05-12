# frozen_string_literal: true
#
# Example: fetch a URL and run an extraction schema against the response.
# Uses Net::HTTP (stdlib) — no external HTTP gem needed.
#
#   ruby -Ilib examples/fetch_and_extract.rb https://example.com/

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "scrapetor"
require "json"

url = ARGV[0] || "https://example.com/"

schema = Scrapetor.schema do
  field :title, from: "title", clean: true
  field :h1s,   from: "h1", multi: true
  field :links, from: "a", attr: :href, multi: true, type: :url, normalize_url: true
end

begin
  result = Scrapetor.fetch_extract(url, schema)
  puts JSON.pretty_generate(result)
rescue Scrapetor::HTTP::FetchError => e
  warn "fetch failed: #{e.message}"
  exit 1
end
