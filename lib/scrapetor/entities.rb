# frozen_string_literal: true

module Scrapetor
  # HTML named-entity decoder.
  #
  # The C native engine handles the minimal set (`&amp; &lt; &gt;
  # &quot; &apos; &nbsp; &#N; &#xH;`) inline during extraction. This
  # module is the broader Ruby table — useful when post-processing raw
  # strings, or when running the Ruby fallback path against HTML with
  # uncommon entities.
  #
  # The table here covers the ~140 most frequent named entities from
  # the HTML5 spec — enough to handle real-world text.
  module Entities
    TABLE = {
      "amp"     => "&",  "lt"      => "<",  "gt"      => ">",
      "quot"    => '"',  "apos"    => "'",  "nbsp"    => " ",
      "copy"    => "©",  "reg"     => "®",  "trade"   => "™",
      "mdash"   => "—",  "ndash"   => "–",  "hellip"  => "…",
      "ldquo"   => "“",  "rdquo"   => "”",  "lsquo"   => "‘",
      "rsquo"   => "’",  "laquo"   => "«",  "raquo"   => "»",
      "lsaquo"  => "‹",  "rsaquo"  => "›",  "sbquo"   => "‚",
      "bdquo"   => "„",  "times"   => "×",  "divide"  => "÷",
      "plusmn"  => "±",  "deg"     => "°",  "sect"    => "§",
      "para"    => "¶",  "middot"  => "·",  "bull"    => "•",
      "dagger"  => "†",  "Dagger"  => "‡",  "permil"  => "‰",
      "prime"   => "′",  "Prime"   => "″",  "ne"      => "≠",
      "le"      => "≤",  "ge"      => "≥",  "asymp"   => "≈",
      "equiv"   => "≡",  "infin"   => "∞",  "sum"     => "∑",
      "prod"    => "∏",  "int"     => "∫",  "radic"   => "√",
      "part"    => "∂",  "nabla"   => "∇",  "minus"   => "−",
      "plus"    => "+",  "lowast"  => "∗",  "frasl"   => "⁄",
      "larr"    => "←",  "rarr"    => "→",  "uarr"    => "↑",
      "darr"    => "↓",  "harr"    => "↔",  "crarr"   => "↵",
      "lArr"    => "⇐",  "rArr"    => "⇒",  "uArr"    => "⇑",
      "dArr"    => "⇓",  "hArr"    => "⇔",  "spades"  => "♠",
      "clubs"   => "♣",  "hearts"  => "♥",  "diams"   => "♦",
      "loz"     => "◊",  "Aacute"  => "Á",  "aacute"  => "á",
      "Acirc"   => "Â",  "acirc"   => "â",  "Agrave"  => "À",
      "agrave"  => "à",  "Aring"   => "Å",  "aring"   => "å",
      "Atilde"  => "Ã",  "atilde"  => "ã",  "Auml"    => "Ä",
      "auml"    => "ä",  "AElig"   => "Æ",  "aelig"   => "æ",
      "Ccedil"  => "Ç",  "ccedil"  => "ç",  "Eacute"  => "É",
      "eacute"  => "é",  "Ecirc"   => "Ê",  "ecirc"   => "ê",
      "Egrave"  => "È",  "egrave"  => "è",  "Euml"    => "Ë",
      "euml"    => "ë",  "Iacute"  => "Í",  "iacute"  => "í",
      "Icirc"   => "Î",  "icirc"   => "î",  "Igrave"  => "Ì",
      "igrave"  => "ì",  "Iuml"    => "Ï",  "iuml"    => "ï",
      "Ntilde"  => "Ñ",  "ntilde"  => "ñ",  "Oacute"  => "Ó",
      "oacute"  => "ó",  "Ocirc"   => "Ô",  "ocirc"   => "ô",
      "Ograve"  => "Ò",  "ograve"  => "ò",  "Oslash"  => "Ø",
      "oslash"  => "ø",  "Otilde"  => "Õ",  "otilde"  => "õ",
      "Ouml"    => "Ö",  "ouml"    => "ö",  "Uacute"  => "Ú",
      "uacute"  => "ú",  "Ucirc"   => "Û",  "ucirc"   => "û",
      "Ugrave"  => "Ù",  "ugrave"  => "ù",  "Uuml"    => "Ü",
      "uuml"    => "ü",  "Yacute"  => "Ý",  "yacute"  => "ý",
      "yuml"    => "ÿ",  "szlig"   => "ß",
      "iexcl"   => "¡",  "iquest"  => "¿",  "cent"    => "¢",
      "pound"   => "£",  "yen"     => "¥",  "euro"    => "€",
      "curren"  => "¤",  "shy"     => "­",
      "frac12"  => "½",  "frac14"  => "¼",  "frac34"  => "¾",
      "alpha"   => "α",  "beta"    => "β",  "gamma"   => "γ",
      "delta"   => "δ",  "epsilon" => "ε",  "pi"      => "π",
      "sigma"   => "σ",  "omega"   => "ω",
      "ensp"    => " ",  "emsp"    => " ",  "thinsp"  => " ",
      "zwnj"    => "‌", "zwj"     => "‍"
    }.freeze

    ENTITY_RE = /&(?:#(?:x([0-9A-Fa-f]+)|(\d+))|([a-zA-Z][a-zA-Z0-9]+));/.freeze

    # Decode a string containing HTML entities into plain UTF-8 text.
    def self.decode(s)
      return s if s.nil? || s.empty?
      s.to_s.gsub(ENTITY_RE) do
        hex = Regexp.last_match(1)
        dec = Regexp.last_match(2)
        named = Regexp.last_match(3)
        if hex
          [hex.to_i(16)].pack("U")
        elsif dec
          [dec.to_i].pack("U")
        elsif named
          TABLE[named] || Regexp.last_match(0)
        else
          Regexp.last_match(0)
        end
      end
    end
  end
end
