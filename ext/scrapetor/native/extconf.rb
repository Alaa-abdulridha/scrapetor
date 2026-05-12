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

create_makefile("scrapetor/scrapetor_native")
