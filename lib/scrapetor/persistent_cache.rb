# frozen_string_literal: true

require "digest"
require "fileutils"

module Scrapetor
  # Disk-backed parse cache. Persists the parsed arena (nodes blob,
  # attrs blob, html bytes) to disk so subsequent process invocations
  # restore the document via memcpy + index rebuild — the SAX
  # tokeniser doesn't run on hit. Implementation is fully native:
  # `Scrapetor::Native::Document#serialize_to_file` writes the binary
  # arena; `Scrapetor::Native::Document.load_from_file` reads it back.
  #
  # Designed for:
  #   - CI / test suites looping the same fixture HTML across boots
  #   - Batch jobs that restart (cron, sidekiq workers)
  #   - A/B parser comparisons over a corpus
  #
  # Storage layout: SCRAP_CACHE_DIR/<first-2-bytes>/<sha256>.arena
  # Files are content-addressed so identical HTML inputs share one
  # cache entry regardless of caller.
  #
  # Opt-in via SCRAP_PERSISTENT_CACHE=1 or Scrapetor::PersistentCache.enable!
  # Override the cache root via SCRAP_CACHE_DIR (default
  # ~/.cache/scrapetor/parse).
  module PersistentCache
    DEFAULT_DIR = File.expand_path("~/.cache/scrapetor/parse")

    class << self
      attr_accessor :dir

      def enabled?
        return @enabled unless @enabled.nil?
        ENV["SCRAP_PERSISTENT_CACHE"] == "1"
      end

      def enable!
        @enabled = true
        @dir   ||= ENV.fetch("SCRAP_CACHE_DIR", DEFAULT_DIR)
        FileUtils.mkdir_p(@dir)
        true
      end

      def disable!
        @enabled = false
      end

      def directory
        @dir ||= ENV.fetch("SCRAP_CACHE_DIR", DEFAULT_DIR)
      end

      # Load a cached parsed arena for the given HTML, or nil on miss.
      # The return value is a Scrapetor::Native::Document ready to be
      # wrapped by Scrapetor::Document.
      def load(html)
        return nil unless enabled?
        return nil if html.nil? || html.empty?
        key = key_for(html)
        path = path_for(key)
        return nil unless File.exist?(path)
        native = Scrapetor::Native::Document.load_from_file(path)
        native
      rescue StandardError
        File.delete(path) rescue nil
        nil
      end

      # Persist a parsed arena to disk under its content fingerprint.
      # Takes the Scrapetor::Native::Document handle (i.e.
      # `doc.backing.native` for an unmutated document). Returns the
      # cache key on success, nil on miss / disabled.
      def store(html, native_doc)
        return nil unless enabled?
        return nil if html.nil? || html.empty?
        return nil if native_doc.nil?
        key = key_for(html)
        path = path_for(key)
        return key if File.exist?(path)
        FileUtils.mkdir_p(File.dirname(path))
        tmp = "#{path}.tmp.#{Process.pid}"
        ok = native_doc.serialize_to_file(tmp)
        unless ok
          File.delete(tmp) rescue nil
          return nil
        end
        File.rename(tmp, path)
        key
      end

      # SHA-256 of the HTML — collisions effectively zero.
      def key_for(html)
        Digest::SHA256.hexdigest(html)
      end

      # Pre-warm the cache for a directory of fixtures.
      def warm(paths_or_globs)
        return 0 unless enabled?
        n = 0
        Array(paths_or_globs).each do |entry|
          Dir.glob(entry).each do |path|
            html = File.read(path)
            doc = Scrapetor.parse(html)
            store(html, doc.backing.native)
            n += 1
          end
        end
        n
      end

      def disk_usage
        return 0 unless File.directory?(directory)
        Dir.glob(File.join(directory, "*", "*.arena")).sum { |p| File.size(p) }
      end

      def clear!
        return 0 unless File.directory?(directory)
        Dir.glob(File.join(directory, "*", "*.arena")).each(&File.method(:delete)).size
      end

      private

      def path_for(key)
        File.join(directory, key[0, 2], "#{key}.arena")
      end
    end

    enable! if enabled?
  end
end
