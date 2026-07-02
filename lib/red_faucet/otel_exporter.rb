# frozen_string_literal: true

require "json"

class RedFaucet
  # Converts a captured event stream into an OTLP/JSON ResourceSpans document.
  #
  # Adapted from Vivarium::OtelExporter (github.com/udzura/vivarium, same
  # author): the OTLP envelope/span-shaping helpers (wrap_document, span
  # hashes, hex-encoded ids, str_attr/int_attr) are carried over largely as
  # is, since that shaping is generic. build_spans itself is new: Vivarium's
  # version reconstructs spans from eBPF span_start/span_stop events carrying
  # their own payload (method name/file/line) and trace/span ids; RedFaucet's
  # trace_event_t is simpler (probe_id + call_id + session_id), so spans are
  # rebuilt here from a per-thread call_id stack instead.
  #
  # This module is shipped as a reference for whoever implements the central
  # daemon (out of scope for this gem) -- see README's XPC protocol section.
  module OtelExporter
    SPAN_KIND_INTERNAL = 1
    SERVICE_NAME = "red_faucet"

    module_function

    # io: a writable IO. Writes a single-line OTLP/JSON document.
    def dump(io, events:, symbol_table:, meta: {})
      io.write(JSON.generate(build_document(events: events, symbol_table: symbol_table, meta: meta)))
    end

    def build_document(events:, symbol_table:, meta: {})
      wrap_document(build_spans(events: events, symbol_table: symbol_table, meta: meta))
    end

    # Wraps a list of OTLP span hashes in the ResourceSpans envelope.
    def wrap_document(spans)
      {
        resourceSpans: [
          {
            resource: { attributes: [str_attr("service.name", SERVICE_NAME)] },
            scopeSpans: [
              {
                scope: { name: SERVICE_NAME, version: RedFaucet::VERSION },
                spans: spans
              }
            ]
          }
        ]
      }
    end

    # events: array of Hashes (or anything responding to #[]) with
    # :timestamp_ns, :thread_id, :event_type (:call/:return), :probe_id,
    # :call_id, :session_id -- the shape RedFaucet._ring_buffer_dump_session
    # returns. symbol_table: Hash probe_id (Integer) => "Class#method"
    # (String), as sent over XPC / returned by RedFaucet._symbol_table.
    def build_spans(events:, symbol_table:, meta: {})
      session_id = meta[:session_id].to_i
      sorted = events.sort_by { |e| e[:timestamp_ns] }
      stacks = Hash.new { |h, k| h[k] = [] } # thread_id => [open span record, ...]
      spans = []

      sorted.each do |ev|
        stack = stacks[ev[:thread_id]]
        case ev[:event_type].to_sym
        when :call
          stack.push(new_span_record(ev, symbol_table, stack.last))
        when :return
          rec = pop_matching(stack, ev[:call_id])
          spans << span_hash(rec, ev[:timestamp_ns], session_id) if rec
        end
      end

      # Any call that never got a matching return (stop() cut the session
      # short, or the call stack overflowed) is force-closed at the last
      # timestamp we saw, rather than silently dropped.
      last_ts = sorted.map { |e| e[:timestamp_ns] }.max || 0
      stacks.each_value do |stack|
        spans << span_hash(stack.pop, last_ts, session_id) until stack.empty?
      end

      spans
    end

    def new_span_record(ev, symbol_table, parent_rec)
      name = symbol_table[ev[:probe_id]] || symbol_table[ev[:probe_id].to_s] || "probe##{ev[:probe_id]}"
      {
        span_id: ev[:call_id], parent_span_id: parent_rec && parent_rec[:span_id],
        name: name, thread_id: ev[:thread_id], start_ns: ev[:timestamp_ns]
      }
    end

    # Pops the record matching call_id (normally the top of the stack). Falls
    # back to popping the top entry if no exact match is found, so a single
    # missing RETURN doesn't wedge every span above it open forever.
    def pop_matching(stack, call_id)
      idx = stack.rindex { |rec| rec[:span_id] == call_id }
      return stack.pop if idx.nil?

      stack.delete_at(idx)
    end

    def span_hash(rec, end_ns, session_id)
      hash = {
        traceId: hex32(session_id, 0),
        spanId: hex16(rec[:span_id]),
        name: rec[:name],
        kind: SPAN_KIND_INTERNAL,
        startTimeUnixNano: rec[:start_ns].to_s,
        endTimeUnixNano: end_ns.to_s,
        attributes: [int_attr("thread.id", rec[:thread_id])]
      }
      hash[:parentSpanId] = hex16(rec[:parent_span_id]) if rec[:parent_span_id]
      hash
    end

    def hex32(hi, lo)
      format("%016x%016x", hi.to_i, lo.to_i)
    end

    def hex16(value)
      format("%016x", value.to_i)
    end

    def str_attr(key, value)
      { key: key, value: { stringValue: value.to_s } }
    end

    def int_attr(key, value)
      { key: key, value: { intValue: value.to_i.to_s } }
    end
  end
end
