# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "minitest/autorun"
require "fileutils"
require "scrapetor"

class TestPersistentCache < Minitest::Test
  HTML = <<~HTML
    <!DOCTYPE html><html><body>
      <div class="r" id="x"><h2>headline</h2><span class="p">$10</span></div>
      <div class="r"><h2>another</h2><span class="p">$20</span></div>
    </body></html>
  HTML

  def setup
    @dir = "/tmp/scrap_pc_test_#{Process.pid}_#{rand(1_000_000)}"
    FileUtils.mkdir_p(@dir)
    ENV["SCRAP_CACHE_DIR"] = @dir
    Scrapetor::PersistentCache.dir = @dir
    Scrapetor::PersistentCache.enable!
  end

  def teardown
    Scrapetor::PersistentCache.disable!
    FileUtils.rm_rf(@dir)
    ENV.delete("SCRAP_CACHE_DIR")
  end

  def test_key_for_is_deterministic_sha256
    k1 = Scrapetor::PersistentCache.key_for(HTML)
    k2 = Scrapetor::PersistentCache.key_for(HTML)
    assert_equal k1, k2
    assert_equal 64, k1.size
    assert_match(/\A[a-f0-9]{64}\z/, k1)
  end

  def test_store_and_load_round_trip
    doc = Scrapetor.parse(HTML)
    key = Scrapetor::PersistentCache.store(HTML, doc.backing.native)
    refute_nil key

    loaded = Scrapetor::PersistentCache.load(HTML)
    refute_nil loaded
    # Reconstruct as a Document and verify it queries correctly.
    doc2 = Scrapetor::Document.new(HTML, native: loaded)
    assert_equal "headline", doc2.at_css("h2").text
    assert_equal "$10",      doc2.at_css(".p").text
    assert_equal 2,          doc2.css(".r").size
  end

  def test_load_returns_nil_on_miss
    refute Scrapetor::PersistentCache.load("<html><body>different</body></html>")
  end

  def test_disabled_load_returns_nil_even_with_entry_on_disk
    doc = Scrapetor.parse(HTML)
    Scrapetor::PersistentCache.store(HTML, doc.backing.native)
    Scrapetor::PersistentCache.disable!
    assert_nil Scrapetor::PersistentCache.load(HTML)
    Scrapetor::PersistentCache.enable!  # restore for teardown
  end

  def test_scrapetor_parse_uses_cache_when_env_set
    # First parse populates the cache.
    Scrapetor.parse(HTML)
    sha = Scrapetor::PersistentCache.key_for(HTML)
    expected_path = File.join(@dir, sha[0, 2], "#{sha}.arena")
    assert File.exist?(expected_path), "cache file should exist at #{expected_path}"
  end

  def test_clear_removes_all_entries
    Scrapetor.parse(HTML)
    Scrapetor::PersistentCache.store("<html>another</html>",
                                       Scrapetor.parse("<html>another</html>").backing.native)
    initial = Scrapetor::PersistentCache.disk_usage
    assert initial > 0
    cleared = Scrapetor::PersistentCache.clear!
    assert_operator cleared, :>=, 1
    assert_equal 0, Scrapetor::PersistentCache.disk_usage
  end

  def test_disk_usage_reflects_stored_bytes
    Scrapetor.parse(HTML)
    sz = Scrapetor::PersistentCache.disk_usage
    assert_operator sz, :>, 0
  end
end
