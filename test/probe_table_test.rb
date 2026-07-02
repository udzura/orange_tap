# frozen_string_literal: true

require "test_helper"

class ProbeTableTest < Test::Unit::TestCase
  include RedFaucetTestCase

  test "trace_method registers a probe and the hook records call/return for it" do
    klass = Class.new do
      def traced_method
        1 + 1
      end

      def untraced_method
        2 + 2
      end
    end
    RedFaucet.trace_method(klass, :traced_method)

    tape = RedFaucet.new
    tape.open
    events = nil
    begin
      instance = klass.new
      instance.traced_method
      instance.untraced_method
      events = RedFaucet._ring_buffer_dump_session(RedFaucet.send(:_active_session_id))
    ensure
      tape.stop
    end

    assert_equal(2, events.size, events.inspect)
    assert_equal(:call, events[0][:event_type])
    assert_equal(:return, events[1][:event_type])
    assert_equal(events[0][:probe_id], events[1][:probe_id])
  end

  test "untrace_method stops the hook from recording it" do
    klass = Class.new do
      def method_a
        :a
      end
    end
    RedFaucet.trace_method(klass, :method_a)
    RedFaucet.untrace_method(klass, :method_a)

    tape = RedFaucet.new
    tape.open
    events = nil
    begin
      klass.new.method_a
      events = RedFaucet._ring_buffer_dump_session(RedFaucet.send(:_active_session_id))
    ensure
      tape.stop
    end

    assert_equal([], events)
  end

  test "trace_all_instance_methods registers every instance method" do
    klass = Class.new do
      def one; end

      def two; end
    end
    RedFaucet.trace_all_instance_methods(klass)

    tape = RedFaucet.new
    tape.open
    events = nil
    begin
      instance = klass.new
      instance.one
      instance.two
      events = RedFaucet._ring_buffer_dump_session(RedFaucet.send(:_active_session_id))
    ensure
      tape.stop
    end

    probe_ids = events.map { |e| e[:probe_id] }.uniq
    assert_equal(2, probe_ids.size)
  end
end
