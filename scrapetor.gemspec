require_relative "lib/scrapetor/version"

Gem::Specification.new do |spec|
  spec.name        = "scrapetor"
  spec.version     = Scrapetor::VERSION
  spec.authors     = ["Alaa Abdulridha"]
  spec.email       = ["alaa@serpapi.com"]

  spec.summary     = "High-performance HTML parsing and structured-data extraction for Ruby."
  spec.description =
    "Scrapetor is a Ruby HTML parsing and scraping toolkit. It pairs a " \
    "native C arena DOM with structural indexes built at parse time and a " \
    "streaming extraction engine that compiles a schema DSL directly to a " \
    "single forward pass over the input — no DOM materialised, one Ruby " \
    "boundary crossing per document. Includes encoding detection, " \
    "structured-data extractors (JSON-LD, OpenGraph, Schema.org, Microdata, " \
    "RDFa, Twitter Cards), a pure-Ruby builder and SAX streamer, a CLI, and " \
    "a minimal HTTP fetcher. No external parser dependency."

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

  # Comparison oracles used by the benchmark scripts only. Not loaded by
  # production code.
  spec.add_development_dependency "nokogiri",      ">= 1.13"
  spec.add_development_dependency "nokolexbor",    ">= 0.6"
end
