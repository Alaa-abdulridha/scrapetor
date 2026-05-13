# frozen_string_literal: true

require "uri"

module Scrapetor
  # HTML form helper. Pulls fields + default values out of a `<form>`
  # element, lets the caller override or add values, and submits via
  # the right method/action.
  #
  #   doc  = Scrapetor::Fetcher.fetch("https://example.com/login")
  #   form = Scrapetor::Form.new(doc.at_css("form#login"),
  #                              base_url: "https://example.com/login")
  #   form["username"] = "alice"
  #   form["password"] = "secret"
  #   resp = form.submit                  # uses Scrapetor::Fetcher
  #
  # Captures every named control's default value (incl. <select> /
  # <input type=hidden|checkbox|radio> / <textarea>); pre-loaded
  # fields like CSRF tokens carry forward automatically. Buttons are
  # NOT included unless explicitly set — the caller decides which
  # submit button "fired".
  class Form
    attr_reader :action, :method, :enctype, :fields

    def initialize(form_node, base_url: nil, http: nil)
      raise ArgumentError, "form_node is required" if form_node.nil?
      @form    = form_node
      @base    = base_url
      @http    = http
      @method  = (form_node["method"] || form_node[:method] || "GET").upcase
      @enctype = (form_node["enctype"] || form_node[:enctype] || "application/x-www-form-urlencoded").downcase
      raw_action = form_node["action"] || form_node[:action] || ""
      @action = if raw_action.empty?
                  base_url
                elsif base_url
                  begin
                    URI.join(base_url, raw_action).to_s
                  rescue URI::InvalidURIError
                    raw_action
                  end
                else
                  raw_action
                end
      @fields = capture_defaults(form_node)
    end

    def [](name);            @fields[name.to_s]; end
    def []=(name, value);    @fields[name.to_s] = value.to_s; end
    def delete(name);        @fields.delete(name.to_s); end
    def merge!(hash);        hash.each { |k, v| self[k] = v }; self; end

    # Returns the params Hash that would be submitted, with all the
    # captured defaults plus user overrides. Useful for inspection
    # before #submit fires the request.
    def to_h
      @fields.dup
    end

    def submit(extra: {}, **fetcher_opts)
      params = @fields.merge(extra.transform_keys(&:to_s))
      client = @http || Scrapetor::Fetcher
      case @method
      when "GET"
        url = append_query(@action, params)
        client.get(url, **fetcher_opts)
      when "POST"
        if @enctype.include?("multipart")
          client.post(@action, multipart: params, **fetcher_opts)
        else
          client.post(@action, form: params, **fetcher_opts)
        end
      else
        # PUT/PATCH/DELETE via form are non-standard but supported.
        verb = @method.downcase.to_sym
        client.send(verb, @action,
                    body: URI.encode_www_form(params),
                    **fetcher_opts.merge(
                      headers: (fetcher_opts[:headers] || {}).merge(
                        "Content-Type" => "application/x-www-form-urlencoded"
                      )
                    ))
      end
    end

    private

    def capture_defaults(form)
      out = {}
      # <input>
      form.css("input").each do |inp|
        name = (inp["name"] || inp[:name])&.to_s
        next if name.nil? || name.empty?
        type = (inp["type"] || inp[:type] || "text").to_s.downcase
        case type
        when "submit", "button", "image", "reset", "file"
          # Skip — submit buttons are caller-driven; file inputs
          # need explicit Fetcher.upload_file via :extra.
          next
        when "checkbox", "radio"
          # Default-checked controls contribute their value; others
          # don't. Falls back to "on" per HTML spec.
          if inp["checked"] || inp[:checked]
            out[name] = (inp["value"] || inp[:value] || "on").to_s
          end
        else
          out[name] = (inp["value"] || inp[:value] || "").to_s
        end
      end
      # <select>
      form.css("select").each do |sel|
        name = (sel["name"] || sel[:name])&.to_s
        next if name.nil? || name.empty?
        # First check for an option marked selected; fall back to
        # the first option (HTML semantics for single-select).
        selected = sel.css("option").find { |o| o["selected"] || o[:selected] }
        selected ||= sel.at_css("option")
        out[name] = selected ? (selected["value"] || selected[:value] || selected.text).to_s : ""
      end
      # <textarea>
      form.css("textarea").each do |t|
        name = (t["name"] || t[:name])&.to_s
        next if name.nil? || name.empty?
        out[name] = t.text.to_s
      end
      out
    end

    def append_query(url, params)
      return url if params.empty?
      uri = URI(url)
      existing = uri.query ? URI.decode_www_form(uri.query) : []
      override_names = params.keys.to_set
      existing.reject! { |k, _| override_names.include?(k) }
      merged = existing + params.to_a
      uri.query = URI.encode_www_form(merged)
      uri.to_s
    end
  end
end

require "set"
