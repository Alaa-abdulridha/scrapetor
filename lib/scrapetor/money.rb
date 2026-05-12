# frozen_string_literal: true

module Scrapetor
  module Money
    NUMERIC = /-?\d[\d.,]*/.freeze
    THOUSAND_GROUPED_COMMAS = /\A-?\d{1,3}(?:,\d{3})+\z/.freeze
    THOUSAND_GROUPED_DOTS = /\A-?\d{1,3}(?:\.\d{3})+\z/.freeze

    def self.parse(s)
      return nil if s.nil?
      m = s.to_s.match(NUMERIC)
      return nil unless m
      num = m[0]
      dots = num.count(".")
      commas = num.count(",")
      if dots > 0 && commas > 0
        if num.rindex(".") > num.rindex(",")
          num = num.delete(",")
        else
          num = num.delete(".").tr(",", ".")
        end
      elsif commas > 0
        num = THOUSAND_GROUPED_COMMAS.match?(num) ? num.delete(",") : num.tr(",", ".")
      elsif dots > 1
        num = THOUSAND_GROUPED_DOTS.match?(num) ? num.delete(".") : num
      end
      num.to_f
    end
  end
end
