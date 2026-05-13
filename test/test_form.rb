# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
$LOAD_PATH.unshift File.expand_path("./support", __dir__)
require "minitest/autorun"
require "scrapetor"

begin
  require "webrick"
  require "local_server"
  TEST_FORM_WEBRICK = true
rescue LoadError
  TEST_FORM_WEBRICK = false
end

class TestForm < Minitest::Test
  # Pure parsing tests (no network)
  def test_captures_text_input_defaults
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input name="user" value="alice">
        <input name="email" value="a@b">
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "alice", form["user"]
    assert_equal "a@b",   form["email"]
  end

  def test_captures_hidden_inputs_like_csrf_tokens
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input type="hidden" name="csrf" value="ABC-123">
        <input name="user" value="">
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "ABC-123", form["csrf"]
    assert_equal "",        form["user"]
  end

  def test_skips_submit_buttons_by_default
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input name="u" value="x">
        <input type="submit" name="action" value="save">
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    refute_includes form.to_h.keys, "action"
  end

  def test_checkbox_default_check_carries_value
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input type="checkbox" name="agree" value="yes" checked>
        <input type="checkbox" name="news"  value="1">
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "yes", form["agree"]
    refute_includes form.to_h.keys, "news"
  end

  def test_radio_default_check_carries_value
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input type="radio" name="plan" value="free">
        <input type="radio" name="plan" value="pro" checked>
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "pro", form["plan"]
  end

  def test_select_default_option
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <select name="color">
          <option value="red">Red</option>
          <option value="blue" selected>Blue</option>
          <option value="green">Green</option>
        </select>
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "blue", form["color"]
  end

  def test_select_first_option_when_none_selected
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <select name="size">
          <option value="s">S</option>
          <option value="m">M</option>
        </select>
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "s", form["size"]
  end

  def test_textarea_default
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <textarea name="message">hello world</textarea>
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "hello world", form["message"]
  end

  def test_user_overrides_replace_defaults
    doc = Scrapetor.parse('<form><input name="u" value="default"></form>')
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    form["u"] = "override"
    assert_equal "override", form["u"]
  end

  def test_merge_sets_many_at_once
    doc = Scrapetor.parse(<<~HTML)
      <form>
        <input name="a" value="">
        <input name="b" value="">
        <input type="hidden" name="csrf" value="X">
      </form>
    HTML
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/").merge!(a: "1", b: "2")
    assert_equal "1", form["a"]
    assert_equal "2", form["b"]
    assert_equal "X", form["csrf"]  # untouched
  end

  def test_action_relative_resolved_against_base_url
    doc = Scrapetor.parse('<form action="/login"></form>')
    form = Scrapetor::Form.new(doc.at_css("form"),
                                base_url: "https://example.com/page")
    assert_equal "https://example.com/login", form.action
  end

  def test_action_empty_falls_back_to_base_url
    doc = Scrapetor.parse('<form></form>')
    form = Scrapetor::Form.new(doc.at_css("form"),
                                base_url: "https://example.com/page")
    assert_equal "https://example.com/page", form.action
  end

  def test_method_defaults_to_get_and_uppercases
    doc = Scrapetor.parse('<form></form>')
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "GET", form.method
  end

  def test_method_post_recognised
    doc = Scrapetor.parse('<form method="post"></form>')
    form = Scrapetor::Form.new(doc.at_css("form"), base_url: "https://x/")
    assert_equal "POST", form.method
  end

  # ---------- Live submission via WEBrick ----------

  def runnable_methods_with_server?
    Scrapetor::Fetcher.available? && TEST_FORM_WEBRICK
  end

  def test_get_form_submission_encodes_in_query_string
    skip "needs libcurl + webrick" unless runnable_methods_with_server?
    server = Scrapetor::TestSupport::LocalServer.new
    captured = nil
    server.mount("/search") do |req, res|
      captured = req.query
      res.body = "ok"; res.status = 200
    end
    begin
      doc = Scrapetor.parse(<<~HTML)
        <form action="#{server.url("/search")}" method="get">
          <input name="q" value="">
          <input type="hidden" name="tag" value="news">
        </form>
      HTML
      form = Scrapetor::Form.new(doc.at_css("form"), base_url: server.url("/"))
      form["q"] = "ruby scrapers"
      form.submit
      assert_equal "ruby scrapers", captured["q"]
      assert_equal "news",          captured["tag"]
    ensure
      server.stop
    end
  end

  def test_post_form_submission_form_encoded
    skip "needs libcurl + webrick" unless runnable_methods_with_server?
    server = Scrapetor::TestSupport::LocalServer.new
    captured = nil
    server.mount("/login") do |req, res|
      captured = { ctype: req["content-type"], body: req.body }
      res.body = "ok"; res.status = 200
    end
    begin
      doc = Scrapetor.parse(<<~HTML)
        <form action="#{server.url("/login")}" method="post">
          <input type="hidden" name="csrf" value="TOKEN-1">
          <input name="user" value="">
          <input name="pass" value="">
        </form>
      HTML
      form = Scrapetor::Form.new(doc.at_css("form"), base_url: server.url("/"))
      form["user"] = "alice"
      form["pass"] = "secret!"
      form.submit
      assert_equal "application/x-www-form-urlencoded", captured[:ctype]
      assert_includes captured[:body], "csrf=TOKEN-1"
      assert_includes captured[:body], "user=alice"
      assert_includes captured[:body], "pass=secret%21"
    ensure
      server.stop
    end
  end
end
