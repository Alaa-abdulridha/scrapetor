# frozen_string_literal: true

# End-to-end fetch + parse + extract benchmark.
#
# Spins up a local WEBrick server, serves a fixture page from disk,
# and measures Scrapetor vs Net::HTTP + Nokogiri vs Net::HTTP +
# Nokolexbor across four scenarios:
#
#   1. cold-parse (string -> DOM -> first extract)
#   2. sequential N-page fetch + parse + extract
#   3. parallel  N-page fetch + parse + extract
#   4. ETag-cache warm 2nd run (Scrapetor only — others have no cache)
#
# All numbers are measured against THE SAME local server with no
# network latency, so they isolate library overhead. Run twice and
# trust the second; first-run includes connection warmup.

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "scrapetor"
require "nokogiri"
require "nokolexbor"
require "net/http"
require "uri"
require "thread"
require "webrick"
require "benchmark"
require "fileutils"

# Generate a SERP-style fixture in memory so the bench is self-contained.
FIXTURE = (lambda do
  buf = +"<!doctype html><html><head><title>Search Results</title></head><body>"
  buf << "<header><nav><a href='/'>Home</a></nav></header><main id='results'>"
  50.times do |i|
    buf << <<~ROW
      <div class="organic-result" data-rank="#{i}">
        <a href="https://example.com/r#{i}">
          <span class="title">Result Title #{i}: Lorem ipsum dolor sit amet</span>
        </a>
        <div class="description">
          Consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et
          dolore magna aliqua. Result number #{i} of #{50} on this synthetic page.
        </div>
        <span class="price">$#{i * 3 + 9}.99</span>
        <span class="rating">#{(4.0 + (i % 10) * 0.1).round(1)}</span>
      </div>
    ROW
  end
  buf << "</main></body></html>"
  buf
end).call
puts "Fixture: #{FIXTURE.bytesize} bytes"

server = WEBrick::HTTPServer.new(
  BindAddress: "127.0.0.1", Port: 0,
  AccessLog: [], Logger: WEBrick::Log.new(File.open(File::NULL, "w"))
)
server.mount_proc "/page" do |req, res|
  etag = '"v1"'
  if req["If-None-Match"] == etag
    res.status = 304
  else
    res.status = 200
    res["ETag"] = etag
    res["Content-Type"] = "text/html; charset=utf-8"
    res.body = FIXTURE
  end
end
port = server.config[:Port]
Thread.new { server.start }
sleep 0.3
url = "http://127.0.0.1:#{port}/page"

# ---- Equivalent extract scripts. We pull the same fields each way ----
def scrapetor_extract(doc)
  doc.css(".organic-result").map do |r|
    { title: r.at_css(".title")&.text,
      url:   r.at_css("a")&.[]("href"),
      price: r.at_css(".price")&.text }
  end
end

def nokogiri_extract(doc)
  doc.css(".organic-result").map do |r|
    { title: r.at_css(".title")&.text,
      url:   r.at_css("a")&.[]("href"),
      price: r.at_css(".price")&.text }
  end
end

def nokolexbor_extract(doc)
  doc.css(".organic-result").map do |r|
    { title: r.at_css(".title")&.text,
      url:   r.at_css("a")&.[]("href"),
      price: r.at_css(".price")&.text }
  end
end

# ---- Net::HTTP fetcher used by Nokogiri / Nokolexbor cases ----
def http_get(url)
  uri = URI(url)
  Net::HTTP.start(uri.host, uri.port) do |h|
    h.get(uri.request_uri).body
  end
end

# ---- Sanity: equivalent results ----
sample = http_get(url)
s_rows = scrapetor_extract(Scrapetor.parse(sample))
n_rows = nokogiri_extract(Nokogiri::HTML(sample))
l_rows = nokolexbor_extract(Nokolexbor::HTML(sample))
raise "sanity mismatch" unless s_rows.size == n_rows.size && s_rows.size == l_rows.size
puts "Sanity: all three engines produce #{s_rows.size} rows."
puts

def measure(label, iters = 20)
  # Warmup
  3.times { yield }
  ts = iters.times.map { Benchmark.realtime { yield } }
  median = ts.sort[ts.size / 2]
  fastest = ts.min
  printf "  %-40s median %7.2f ms  best %7.2f ms\n",
         label, median * 1000, fastest * 1000
  median
end

# ---- 1. Cold parse (no I/O) ----
puts "[1] Cold parse + extract (string -> DOM -> rows)"
sp = measure("scrapetor.parse + extract")        { scrapetor_extract(Scrapetor.parse(FIXTURE)) }
nk = measure("nokogiri.parse + extract")          { nokogiri_extract(Nokogiri::HTML(FIXTURE)) }
nx = measure("nokolexbor.parse + extract")        { nokolexbor_extract(Nokolexbor::HTML(FIXTURE)) }
printf "  scrapetor / nokogiri:    %5.2fx %s\n", nk/sp, sp < nk ? "faster" : "slower"
printf "  scrapetor / nokolexbor:  %5.2fx %s\n", nx/sp, sp < nx ? "faster" : "slower"

puts
puts "[2] Sequential fetch + parse + extract (1 process, no parallelism)"
N = 16
sp = measure("scrapetor.fetch + parse + extract") {
  N.times { scrapetor_extract(Scrapetor::Fetcher.fetch(url)) }
}
nk = measure("Net::HTTP + Nokogiri + extract") {
  N.times { nokogiri_extract(Nokogiri::HTML(http_get(url))) }
}
nx = measure("Net::HTTP + Nokolexbor + extract") {
  N.times { nokolexbor_extract(Nokolexbor::HTML(http_get(url))) }
}
printf "  scrapetor / nokogiri:    %5.2fx %s\n", nk/sp, sp < nk ? "faster" : "slower"
printf "  scrapetor / nokolexbor:  %5.2fx %s\n", nx/sp, sp < nx ? "faster" : "slower"

puts
puts "[3] Parallel fetch + parse + extract (8 threads)"
urls = Array.new(N, url)
sp = measure("scrapetor.parallel_fetch + extract") {
  Scrapetor::Fetcher.parallel_fetch(urls, threads: 8).map { |d| scrapetor_extract(d) }
}
nk = measure("Thread.new × Net::HTTP × Nokogiri") {
  urls.each_slice(N/8).flat_map { |slice|
    slice.map { |u| Thread.new { nokogiri_extract(Nokogiri::HTML(http_get(u))) } }.map(&:value)
  }
}
nx = measure("Thread.new × Net::HTTP × Nokolexbor") {
  urls.each_slice(N/8).flat_map { |slice|
    slice.map { |u| Thread.new { nokolexbor_extract(Nokolexbor::HTML(http_get(u))) } }.map(&:value)
  }
}
printf "  scrapetor / nokogiri:    %5.2fx %s\n", nk/sp, sp < nk ? "faster" : "slower"
printf "  scrapetor / nokolexbor:  %5.2fx %s\n", nx/sp, sp < nx ? "faster" : "slower"

puts
puts "[4] ETag cache warm (Scrapetor only; competitors have no built-in cache)"
cache_dir = "/tmp/scrapetor_bench_cache"
FileUtils.rm_rf(cache_dir); FileUtils.mkdir_p(cache_dir)
# Prime the cache
Scrapetor::Fetcher.get(url, cache_dir: cache_dir)
sp_cached = measure("scrapetor.fetch (cached, 304)") {
  N.times { scrapetor_extract(Scrapetor::Fetcher.fetch(url, cache_dir: cache_dir)) }
}
FileUtils.rm_rf(cache_dir)
printf "  cached / fresh-scrapetor:  %5.2fx %s\n",
       sp / sp_cached, sp_cached < sp ? "faster" : "slower"

server.shutdown
