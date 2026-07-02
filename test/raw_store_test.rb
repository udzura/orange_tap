# frozen_string_literal: true

require "test_helper"
require "stringio"

class RawStoreTest < Test::Unit::TestCase
  test "dump/load round-trips events losslessly" do
    events = [
      RedFaucet::RawEvent.new(timestamp_ns: 1_000, thread_id: 1, event_type: :call, probe_id: 7,
                               call_id: 1, session_id: 42),
      RedFaucet::RawEvent.new(timestamp_ns: 2_000, thread_id: 1, event_type: :return, probe_id: 7,
                               call_id: 1, session_id: 42)
    ]

    io = StringIO.new
    RedFaucet::RawStore.dump(io, events: events, meta: { session_id: 42 })

    io.rewind
    result = RedFaucet::RawStore.load(io)

    assert_equal("red_faucet-raw", result[:meta][:format])
    assert_equal(42, result[:meta][:session_id])
    assert_equal(events, result[:events])
  end

  test "load raises FormatError for a header from a different format" do
    io = StringIO.new(%({"format":"something-else"}\n))
    assert_raise(RedFaucet::RawStore::FormatError) { RedFaucet::RawStore.load(io) }
  end

  test "load raises FormatError for an empty file" do
    assert_raise(RedFaucet::RawStore::FormatError) { RedFaucet::RawStore.load(StringIO.new("")) }
  end
end
