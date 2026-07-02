# frozen_string_literal: true

require "json"
require "tmpdir"

require_relative "red_faucet/version"
require_relative "red_faucet/raw_store"
require_relative "red_faucet/otel_exporter"
require "red_faucet_ext"

# See ext/red_faucet/red_faucet.c for the native pieces: the TracePoint-based
# hook, the probe_table (static "what to trace" registry) and session.c (the
# dynamic "when to record" state). This file only adds the Ruby-level public
# API on top of the small `_foo` C bindings.
class RedFaucet
  class << self
    # RedFaucet.configure(mach_service_name: "com.example.tracer_daemon",
    #                     ring_buffer_capacity: 65536)
    def configure(mach_service_name: nil, ring_buffer_capacity: 65_536)
      unless ring_buffer_capacity.positive? && (ring_buffer_capacity & (ring_buffer_capacity - 1)).zero?
        raise ArgumentError, "ring_buffer_capacity must be a power of two, got #{ring_buffer_capacity}"
      end

      _configure(mach_service_name, ring_buffer_capacity, RUBY_VERSION)
    end

    # RedFaucet.trace_method(SomeClass, :some_method)
    def trace_method(klass, method_name)
      _trace_method(klass, method_name.to_sym)
    end

    # RedFaucet.trace_all_instance_methods(SomeClass)
    def trace_all_instance_methods(klass)
      methods = klass.instance_methods(false) + klass.private_instance_methods(false)
      methods.each { |m| trace_method(klass, m) }
    end

    # RedFaucet.untrace_method(SomeClass, :some_method)
    def untrace_method(klass, method_name)
      _untrace_method(klass, method_name.to_sym)
    end

    # RedFaucet.open do
    #   ...
    # end # => path to the generated Span JSON file
    #
    # A thin syntactic-sugar wrapper around the instance form (#open/#stop) so
    # the two never duplicate session lifecycle logic (DESIGN.md 3.7).
    def open
      raise ArgumentError, "block required" unless block_given?

      tape = new
      tape.open
      path = nil
      begin
        yield
      ensure
        path = tape.stop
      end
      path
    end

    private

    # ---------------------------------------------------------------------
    # Test-only: an in-process stand-in for the (out of scope) central
    # daemon. See ext/red_faucet/mock_daemon.c for why "stop_session"
    # requests are bridged to this Ruby thread over plain pipes rather than
    # calling back into Ruby directly from the XPC handler's own thread.
    # ---------------------------------------------------------------------

    def _enable_test_mock_daemon!
      req_fd, resp_fd = _start_test_mock_daemon
      thread = Thread.new { _mock_daemon_bridge_loop(req_fd, resp_fd) }
      thread.name = "red_faucet-mock-daemon"
      thread.abort_on_exception = true
      thread
    end

    def _mock_daemon_bridge_loop(req_fd, resp_fd)
      req_io = IO.for_fd(req_fd, autoclose: false)
      resp_io = IO.for_fd(resp_fd, autoclose: false)
      resp_io.sync = true # unbuffered: the C side blocks on read() waiting for these bytes

      loop do
        data = req_io.read(8)
        break if data.nil?

        session_id = data.unpack1("Q<")
        begin
          path = _mock_daemon_build_span_file(session_id)
          write_bridge_response(resp_io, ok: true, payload: path)
        rescue StandardError => e
          write_bridge_response(resp_io, ok: false, payload: e.message.to_s)
        end
      end
    end

    def write_bridge_response(resp_io, ok:, payload:)
      bytes = payload.to_s.b
      resp_io.write([ok ? 1 : 0].pack("C"))
      resp_io.write([bytes.bytesize].pack("L<"))
      resp_io.write(bytes)
    end

    # Stands in for "the daemon ends session_id, converts it to OTel Spans,
    # and returns the resulting file's path" -- the one piece of daemon
    # behavior this gem needs to fake to test RedFaucet.open/#stop's XPC
    # round trip end to end. Real daemons are free to reuse
    # RedFaucet::RawStore / RedFaucet::OtelExporter the same way.
    def _mock_daemon_build_span_file(session_id)
      events = _ring_buffer_dump_session(session_id)
      symbol_table = _symbol_table

      raw_events = events.map { |ev| RedFaucet::RawEvent.new(**ev) }
      raw_path = File.join(Dir.tmpdir, "red_faucet-#{session_id}-#{Process.pid}.raw")
      File.open(raw_path, "wb") do |f|
        RedFaucet::RawStore.dump(f, events: raw_events, meta: { session_id: session_id })
      end

      document = RedFaucet::OtelExporter.build_document(
        events: events, symbol_table: symbol_table, meta: { session_id: session_id }
      )
      json_path = File.join(Dir.tmpdir, "red_faucet-#{session_id}-#{Process.pid}.json")
      File.write(json_path, JSON.generate(document))
      json_path
    end
  end
end

at_exit { RedFaucet.send(:_notify_exit) }
