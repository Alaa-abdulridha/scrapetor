# frozen_string_literal: true

require "mkmf"

$CFLAGS  << " -std=c99 -O3 -Wall -fno-strict-aliasing"
$CFLAGS  << " -fvisibility=hidden"
$CFLAGS  << " -DNDEBUG"

# Architecture-specific flags
arch = `uname -m`.strip
case arch
when "arm64", "aarch64"
  # NEON is implicit on aarch64; nothing to add
when "x86_64", "amd64"
  $CFLAGS << " -msse4.2"
end

# Ruby version compatibility shim defines
$CFLAGS << " -DRUBY_VERSION_MAJOR=#{RbConfig::CONFIG["MAJOR"]}"

# Optional libcurl-backed HTTP layer. Tries pkg-config first (the
# standard libcurl install carries a .pc file); falls back to header
# + library probes. Defines HAVE_LIBCURL when both succeed. The HTTP
# module compiles to a stub otherwise so the rest of the gem still
# loads cleanly.
have_libcurl = false
unless ENV["SCRAP_NO_LIBCURL"] == "1"
  if pkg_config("libcurl")
    have_libcurl = true
  elsif have_header("curl/curl.h") && have_library("curl", "curl_easy_init")
    have_libcurl = true
  end
end
$defs << "-DHAVE_LIBCURL" if have_libcurl

# zlib for gzip/deflate decoding. Almost universally available;
# libcurl already pulls it in on most systems. We need direct access
# so we can drive decompression ourselves rather than relying on
# libcurl's CURLOPT_ACCEPT_ENCODING (which rejects responses with
# encodings libcurl wasn't compiled for, before our brotli/zstd
# fallback can run).
if have_libcurl
  if pkg_config("zlib") ||
     (have_header("zlib.h") && have_library("z", "inflateInit_"))
    $defs << "-DHAVE_ZLIB"
  end
end

# Optional brotli + zstd in-process decoders. When the linked libcurl
# wasn't built with these (e.g. macOS system libcurl as of 8.7.1),
# Scrapetor can still advertise br/zstd in Accept-Encoding and
# decode the response body itself. Each decoder is opt-in via the
# corresponding library probe; missing libraries downgrade silently.
if have_libcurl && ENV["SCRAP_NO_BROTLI"] != "1"
  if pkg_config("libbrotlidec") ||
     (have_header("brotli/decode.h") && have_library("brotlidec", "BrotliDecoderDecompress"))
    $defs << "-DHAVE_BROTLI"
  end
end
if have_libcurl && ENV["SCRAP_NO_ZSTD"] != "1"
  if pkg_config("libzstd") ||
     (have_header("zstd.h") && have_library("zstd", "ZSTD_decompress"))
    $defs << "-DHAVE_ZSTD"
  end
end

create_makefile("scrapetor/scrapetor_native")
