# frozen_string_literal: true

require "test_helper"

class OtelExporterTest < Test::Unit::TestCase
  test "build_document turns a CALL/RETURN pair into an OTLP span" do
    events = [
      { timestamp_ns: 1_000, thread_id: 1, event_type: :call, probe_id: 1, call_id: 10, session_id: 5 },
      { timestamp_ns: 2_000, thread_id: 1, event_type: :return, probe_id: 1, call_id: 10, session_id: 5 }
    ]
    symbol_table = { 1 => "Foo#bar" }

    document = RedFaucet::OtelExporter.build_document(events: events, symbol_table: symbol_table,
                                                        meta: { session_id: 5 })

    spans = document[:resourceSpans][0][:scopeSpans][0][:spans]
    assert_equal(1, spans.size)
    span = spans[0]
    assert_equal("Foo#bar", span[:name])
    assert_equal("1000", span[:startTimeUnixNano])
    assert_equal("2000", span[:endTimeUnixNano])
  end

  test "a call without a matching return is force-closed at the last timestamp" do
    events = [
      { timestamp_ns: 1_000, thread_id: 1, event_type: :call, probe_id: 1, call_id: 10, session_id: 5 }
    ]

    document = RedFaucet::OtelExporter.build_document(events: events, symbol_table: { 1 => "Foo#bar" },
                                                        meta: { session_id: 5 })

    span = document[:resourceSpans][0][:scopeSpans][0][:spans][0]
    assert_equal("1000", span[:startTimeUnixNano])
    assert_equal("1000", span[:endTimeUnixNano])
  end
end
