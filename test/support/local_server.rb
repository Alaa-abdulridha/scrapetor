# frozen_string_literal: true

require "webrick"

# Shared WEBrick helper for tests that need a real HTTP target. Spawns
# a server on an ephemeral port in a background thread; the caller
# mounts handlers and tears down via #stop. WEBrick's mount_proc only
# routes GET in older Rubies, so we register a servlet that also
# handles POST/PUT/PATCH/DELETE/HEAD.
module Scrapetor
  module TestSupport
    class LocalServer
      class AnyMethod < WEBrick::HTTPServlet::AbstractServlet
        def initialize(server, blk)
          super(server)
          @blk = blk
        end

        %w[GET POST PUT DELETE HEAD].each do |m|
          define_method("do_#{m}") { |req, res| @blk.call(req, res) }
        end

        # WEBrick 1.6 doesn't auto-route PATCH; manual.
        def do_PATCH(req, res); @blk.call(req, res); end
      end

      def initialize
        @server = WEBrick::HTTPServer.new(
          BindAddress: "127.0.0.1", Port: 0,
          AccessLog: [],
          Logger: WEBrick::Log.new(File.open(File::NULL, "w"))
        )
        @thread = Thread.new { @server.start }
        sleep 0.05 until @server.config[:Port] > 0 || !@thread.alive?
      end

      def mount(path, &block)
        @server.mount(path, AnyMethod, block)
        self
      end

      def url(path = "/")
        "http://127.0.0.1:#{@server.config[:Port]}#{path}"
      end

      def stop
        @server.shutdown
        @thread.join(2)
      end
    end
  end
end
