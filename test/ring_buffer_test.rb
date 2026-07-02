# frozen_string_literal: true

require "test_helper"

class RingBufferTest < Test::Unit::TestCase
  include RedFaucetTestCase

  test "push and dump preserve field values and CALL/RETURN order" do
    session_id = 4242
    RedFaucet._ring_buffer_push_raw(session_id, 7, :call, 111, 1, 1_000)
    RedFaucet._ring_buffer_push_raw(session_id, 7, :return, 111, 1, 2_000)

    events = RedFaucet._ring_buffer_dump_session(session_id)

    assert_equal(2, events.size)
    call_event, return_event = events
    assert_equal(:call, call_event[:event_type])
    assert_equal(1_000, call_event[:timestamp_ns])
    assert_equal(:return, return_event[:event_type])
    assert_equal(2_000, return_event[:timestamp_ns])
    assert_equal(7, call_event[:probe_id])
    assert_equal(111, call_event[:thread_id])
    assert_equal(session_id, call_event[:session_id])
  end

  test "different session_ids do not leak into each other's dump" do
    RedFaucet._ring_buffer_push_raw(101, 1, :call, 1, 1, 1)
    RedFaucet._ring_buffer_push_raw(202, 1, :call, 1, 2, 2)

    assert_equal(1, RedFaucet._ring_buffer_dump_session(101).size)
    assert_equal(1, RedFaucet._ring_buffer_dump_session(202).size)
  end

  test "dropped_count increases once writes exceed capacity" do
    RedFaucet.configure(mach_service_name: "__test_mock__", ring_buffer_capacity: 8)

    8.times { |i| RedFaucet._ring_buffer_push_raw(1, 1, :call, 1, i, i) }
    _write, _read, _capacity, dropped_before_overflow = RedFaucet._ring_buffer_stats
    assert_equal(0, dropped_before_overflow)

    3.times { |i| RedFaucet._ring_buffer_push_raw(1, 1, :call, 1, 8 + i, 8 + i) }
    _write, _read, capacity, dropped_after_overflow = RedFaucet._ring_buffer_stats
    assert_equal(8, capacity)
    assert_equal(3, dropped_after_overflow)
  end
end
