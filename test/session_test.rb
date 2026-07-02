# frozen_string_literal: true

require "test_helper"

class SessionTest < Test::Unit::TestCase
  include RedFaucetTestCase

  test "RedFaucet.open returns the generated span file path as a String" do
    klass = Class.new do
      def ping
        :pong
      end
    end
    RedFaucet.trace_method(klass, :ping)

    path = RedFaucet.open { klass.new.ping }

    assert_kind_of(String, path)
    assert(File.exist?(path))
  end

  test "events outside an open/.stop region are not recorded (session_id 0 is skipped)" do
    klass = Class.new do
      def ping
        :pong
      end
    end
    RedFaucet.trace_method(klass, :ping)

    klass.new.ping # no active session yet: must not be recorded anywhere

    tape = RedFaucet.new
    tape.open
    session_id = RedFaucet.send(:_active_session_id)
    events = RedFaucet._ring_buffer_dump_session(session_id)
    tape.stop

    assert_equal([], events)
  end

  test "opening a session while one is already active raises AlreadyRecordingError" do
    tape = RedFaucet.new
    tape.open
    begin
      assert_raise(RedFaucet::AlreadyRecordingError) { RedFaucet.new.open }
    ensure
      tape.stop
    end
  end

  test "stop after stop raises NotRecordingError" do
    tape = RedFaucet.new
    tape.open
    tape.stop
    assert_raise(RedFaucet::NotRecordingError) { tape.stop }
  end

  test "an exception inside RedFaucet.open's block still runs stop via ensure" do
    assert_equal(0, RedFaucet.send(:_active_session_id))

    assert_raise(RuntimeError) do
      RedFaucet.open { raise "boom" }
    end

    assert_equal(0, RedFaucet.send(:_active_session_id))
    # the session must have been detached (not left dangling), so a brand-new
    # one can be opened right away.
    tape = RedFaucet.new
    tape.open
    tape.stop
  end
end
