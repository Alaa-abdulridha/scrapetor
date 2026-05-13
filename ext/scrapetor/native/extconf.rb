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

create_makefile("scrapetor/scrapetor_native")
