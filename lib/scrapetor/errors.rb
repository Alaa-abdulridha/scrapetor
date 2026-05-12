# frozen_string_literal: true

module Scrapetor
  # Base for all Scrapetor errors.
  class Error < StandardError; end

  # Raised when a required field in the schema can't be found.
  class ExtractionError < Error; end

  # Raised when a schema descriptor isn't valid for the native engine.
  class SchemaError < Error; end
end
