# frozen_string_literal: true

module Scrapetor
  module Cleaner
    def self.clean(s)
      return nil if s.nil?
      s.to_s.gsub(/\s+/, " ").strip
    end
  end
end
