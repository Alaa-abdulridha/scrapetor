require_relative "lib/scrapetor/version"

Gem::Specification.new do |spec|
  spec.name        = "scrapetor"
  spec.version     = Scrapetor::VERSION
  spec.authors     = ["Alaa Abdulridha"]
  spec.email       = ["alaa@serpapi.com"]

  spec.summary     = "Production HTML parser + scraping toolkit. Native arena DOM, HTTP/2 fetch layer, streaming extraction."
  spec.description =
    "Scrapetor is a Ruby HTML parsing + scraping toolkit. The parser is a " \
    "native C arena DOM with structural indexes built at parse time and " \
    "NEON SIMD scanners in the SAX hot loop. A streaming extraction engine " \
    "compiles the schema DSL into a single forward pass — no DOM " \
    "materialised, one Ruby boundary crossing per document. " \
    "On builds where libcurl is available, Scrapetor::Fetcher adds an " \
    "HTTP/2-capable fetch layer with per-thread connection cache, shared " \
    "DNS + TLS session pool, in-process gzip / deflate / brotli / zstd " \
    "decoding, iconv charset transcoding, retry + exponential backoff, " \
    "ETag / Last-Modified disk cache with bulk revalidation, per-host " \
    "throttle, cookie jar, basic + bearer auth, proxy, and three bulk " \
    "concurrency models (parallel_fetch / multi_fetch / streaming " \
    "multi_each). Scrapetor::Session ties the cookie / auth / throttle / " \
    "retry policies together. Also ships robots.txt + sitemap.xml " \
    "parsers, a bounded-memory streaming HTML parser, and structured-data " \
    "extractors (JSON-LD, OpenGraph, Schema.org, Microdata, RDFa, Twitter " \
    "Cards). The Net::HTTP-based Scrapetor.fetch is preserved as the " \
    "no-libcurl fallback."

  spec.homepage    = "https://scrapetor.org"
  spec.license     = "MIT"
  spec.required_ruby_version     = ">= 2.7.0"
  spec.required_rubygems_version = ">= 3.0.0"

  spec.metadata = {
    "homepage_uri"          => "https://scrapetor.org",
    "source_code_uri"       => "https://github.com/Alaa-abdulridha/scrapetor",
    "bug_tracker_uri"       => "https://github.com/Alaa-abdulridha/scrapetor/issues",
    "changelog_uri"         => "https://github.com/Alaa-abdulridha/scrapetor/blob/main/CHANGELOG.md",
    "documentation_uri"     => "https://scrapetor.org/docs",
    "rubygems_mfa_required" => "true"
  }

  spec.files = Dir[
    "lib/**/*.rb",
    "ext/**/*.{rb,c,h}",
    "ext/**/README.md",
    "bin/*",
    "CHANGELOG.md",
    "LICENSE",
    "README.md",
    "scrapetor.gemspec"
  ]

  spec.bindir        = "bin"
  spec.executables   = ["scrapetor", "scrapetor-bench"]
  spec.require_paths = ["lib"]
  spec.extensions    = ["ext/scrapetor/native/extconf.rb"]

  # No runtime gem dependencies. Scrapetor is self-contained: pure Ruby
  # plus a single C99 extension. The extension compiles at install time
  # via the standard mkmf path; only a working C compiler is required.

  spec.add_development_dependency "minitest",      "~> 5.0"
  spec.add_development_dependency "benchmark-ips", "~> 2.0"
  spec.add_development_dependency "rake",          "~> 13.0"
  # webrick was bundled with Ruby 2.7 / earlier; removed from stdlib
  # in 3.0. The Fetcher + Session test suites spin up local HTTP
  # servers via it.
  spec.add_development_dependency "webrick",       "~> 1.7"

  # Comparison oracles used by the benchmark scripts only. Not loaded by
  # production code.
  spec.add_development_dependency "nokogiri",      ">= 1.13"
  spec.add_development_dependency "nokolexbor",    ">= 0.6"
end
