# frozen_string_literal: true

module Scrapetor
  # Encoding detection + UTF-8 normalization.
  #
  # The native streaming engine treats the input as a byte stream and tags
  # output strings as UTF-8. To make that honest, we transcode non-UTF-8
  # input to UTF-8 in Ruby before handing it to C — using the cascade the
  # HTML5 spec describes:
  #
  #   1. BOM         — UTF-8 / UTF-16 BE/LE
  #   2. <meta charset=...> in the first ~1024 bytes
  #   3. <meta http-equiv="Content-Type" content="...; charset=...">
  #   4. Fall back to UTF-8
  #
  # If the detected encoding equals UTF-8 (or close enough), we leave the
  # bytes alone. Otherwise we transcode with `invalid: :replace,
  # undef: :replace` so a single bad byte doesn't poison the whole document.
  module Encoding
    META_CHARSET_RE = /<meta[^>]+charset\s*=\s*["']?([A-Za-z0-9_\-:]+)/i.freeze
    META_HTTP_EQUIV_RE = /<meta[^>]+http-equiv\s*=\s*["']?content-type["']?[^>]+content\s*=\s*["'][^"'>]*charset=([A-Za-z0-9_\-:]+)/i.freeze
    SNIFF_BYTES = 1024

    def self.detect(bytes)
      return "UTF-8" if bytes.nil? || bytes.empty?
      head = (bytes.byteslice(0, 4) || "").dup.force_encoding(::Encoding::ASCII_8BIT)
      return "UTF-8"     if head.start_with?("\xEF\xBB\xBF".b)
      return "UTF-32LE"  if head.bytesize >= 4 && head.start_with?("\xFF\xFE\x00\x00".b)
      return "UTF-32BE"  if head.bytesize >= 4 && head.start_with?("\x00\x00\xFE\xFF".b)
      return "UTF-16LE"  if head.bytesize >= 2 && head.byteslice(0, 2) == "\xFF\xFE".b
      return "UTF-16BE"  if head.bytesize >= 2 && head.byteslice(0, 2) == "\xFE\xFF".b
      prefix = (bytes.byteslice(0, SNIFF_BYTES) || "").dup.force_encoding(::Encoding::ASCII_8BIT)
      if (m = prefix.match(META_CHARSET_RE))
        return normalize(m[1])
      end
      if (m = prefix.match(META_HTTP_EQUIV_RE))
        return normalize(m[1])
      end
      "UTF-8"
    end

    def self.normalize(name)
      n = name.to_s.upcase.gsub(/[^A-Z0-9]/, "")
      case n
      when "UTF8", "UTF8N"                                then "UTF-8"
      when "LATIN1", "ISO88591", "WINDOWS1252", "WIN1252", "CP1252"
        "WINDOWS-1252"
      when "SHIFTJIS", "SJIS"                             then "Shift_JIS"
      when "EUCJP"                                        then "EUC-JP"
      when "GBK", "GB2312", "CP936"                       then "GBK"
      when "BIG5"                                         then "Big5"
      when "UTF16", "UTF16LE"                             then "UTF-16LE"
      when "UTF16BE"                                      then "UTF-16BE"
      when "USASCII", "ASCII"                             then "US-ASCII"
      else name.to_s.upcase
      end
    end

    # Best-effort transcode of `bytes` to a UTF-8 String. Strips a leading
    # BOM. Never raises — invalid sequences become "" (dropped).
    BOM_UTF8 = "\xEF\xBB\xBF".b.freeze

    def self.to_utf8(bytes)
      s = bytes.is_a?(String) ? bytes.dup : bytes.to_s
      enc = detect(s)
      s.force_encoding(::Encoding::ASCII_8BIT)
      # Strip UTF-8 BOM if present
      if s.bytesize >= 3 && s.byteslice(0, 3) == BOM_UTF8
        s = s.byteslice(3, s.bytesize - 3) || ""
      end
      if enc.casecmp("UTF-8").zero?
        s.force_encoding(::Encoding::UTF_8)
        return s if s.valid_encoding?
        return s.encode(::Encoding::UTF_8, ::Encoding::UTF_8, invalid: :replace, undef: :replace, replace: "")
      end
      begin
        s.force_encoding(enc)
        s.encode(::Encoding::UTF_8, invalid: :replace, undef: :replace, replace: "")
      rescue ::Encoding::ConverterNotFoundError, ArgumentError
        s.force_encoding(::Encoding::UTF_8)
        s.encode(::Encoding::UTF_8, ::Encoding::UTF_8, invalid: :replace, undef: :replace, replace: "")
      end
    end
  end
end
