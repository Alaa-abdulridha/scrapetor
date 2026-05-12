# frozen_string_literal: true

# Generates the canonical wedge fixture: a 50-product-card e-commerce
# listing page. Run this once; commit the resulting HTML.

require "fileutils"

OUT = File.expand_path("ecommerce_listing.html", __dir__)
FileUtils.mkdir_p(File.dirname(OUT))

cards = (1..50).map do |i|
  price = format("%.2f", 9.99 + i * 3.14)
  rating = format("%.1f", 3.0 + (i % 5) * 0.3)
  <<~CARD
    <article class="product-card" data-sku="SKU#{format("%04d", i)}">
      <a href="/products/widget-#{i}" class="card-link" data-tracking="card-#{i}">
        <img class="product-image" src="/img/widget-#{i}.jpg" alt="Widget #{i}" loading="lazy">
        <div class="product-body">
          <h3 class="product-title">Widget Model #{i}</h3>
          <p class="product-subtitle">Premium-grade widget number #{i}</p>
          <div class="product-meta">
            <span class="price">$#{price}</span>
            <span class="rating" data-rating="#{rating}">#{rating} stars</span>
          </div>
          <ul class="badges">
            <li class="badge new">New</li>
            <li class="badge sale">Sale</li>
            <li class="badge stock">In stock</li>
          </ul>
        </div>
      </a>
    </article>
  CARD
end.join("\n")

html = <<~HTML
  <!DOCTYPE html>
  <html lang="en">
    <head>
      <meta charset="utf-8">
      <title>All Widgets — Acme Co.</title>
      <link rel="canonical" href="https://example.com/widgets">
      <script type="application/ld+json">{"@context":"https://schema.org","@type":"ItemList"}</script>
      <style>.hidden{display:none}</style>
    </head>
    <body>
      <header class="site-header">
        <nav class="primary-nav">
          <ul>
            <li><a href="/">Home</a></li>
            <li><a href="/widgets">Widgets</a></li>
            <li><a href="/about">About</a></li>
          </ul>
        </nav>
      </header>
      <main id="main">
        <h1>All Widgets</h1>
        <section class="product-grid">
          #{cards}
        </section>
      </main>
      <footer class="site-footer">
        <p>&copy; Acme Co.</p>
        <script>console.log("noise")</script>
      </footer>
    </body>
  </html>
HTML

File.write(OUT, html)
puts "Wrote #{OUT} (#{html.bytesize} bytes, 50 product cards)"
