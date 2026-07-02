# frozen_string_literal: true

require "json"

class RedFaucet
  # Mirrors ext/red_faucet/ring_buffer.h's trace_event_t (40 bytes, fixed
  # layout): timestamp_ns:Q< thread_id:L< event_type:L< probe_id:Q< call_id:Q<
  # session_id:Q<. event_type is kept as :call/:return here for convenience;
  # RawStore packs it to the underlying 0/1 on disk.
  RawEvent = Struct.new(
    :timestamp_ns, :thread_id, :event_type, :probe_id, :call_id, :session_id,
    keyword_init: true
  )

  # On-disk raw capture format, adapted from Vivarium::RawStore
  # (github.com/udzura/vivarium, same author): a single JSON metadata line
  # followed by fixed-size binary event_t records, so a capture round-trips
  # losslessly without needing the OTel conversion to happen up front. The
  # real central daemon (out of scope for this gem) is expected to use the
  # same "raw capture now, convert later" split -- see RedFaucet::OtelExporter
  # for the second half.
  module RawStore
    class FormatError < StandardError; end

    FORMAT = "red_faucet-raw"
    VERSION = 1
    EVENT_STRUCT_SIZE = 40
    PACK_FMT = "Q<L<L<Q<Q<Q<"

    EVENT_TYPE_TO_INT = { call: 0, return: 1 }.freeze
    INT_TO_EVENT_TYPE = EVENT_TYPE_TO_INT.invert.freeze

    def self.pack_record(ev)
      [
        ev.timestamp_ns, ev.thread_id, EVENT_TYPE_TO_INT.fetch(ev.event_type.to_sym),
        ev.probe_id, ev.call_id, ev.session_id
      ].pack(PACK_FMT)
    end

    def self.unpack_record(bytes)
      bytes = bytes.to_s.b
      bytes = bytes.ljust(EVENT_STRUCT_SIZE, "\x00") if bytes.bytesize < EVENT_STRUCT_SIZE

      ts, tid, event_type_int, probe_id, call_id, session_id = bytes.unpack(PACK_FMT)
      RawEvent.new(
        timestamp_ns: ts, thread_id: tid,
        event_type: INT_TO_EVENT_TYPE.fetch(event_type_int, :call),
        probe_id: probe_id, call_id: call_id, session_id: session_id
      )
    end

    # io: a binary-writable IO. meta: session metadata Hash.
    def self.dump(io, events:, meta:)
      header = meta.merge(
        format: FORMAT, version: VERSION,
        event_struct_size: EVENT_STRUCT_SIZE, event_count: events.size
      )
      io.binmode
      io.write(JSON.generate(header))
      io.write("\n")
      events.each { |ev| io.write(pack_record(ev)) }
    end

    # Returns { meta: Hash(symbol keys), events: [RawEvent, ...] }.
    def self.load(io)
      io.binmode
      line = io.gets
      raise FormatError, "empty file" if line.nil?

      begin
        meta = JSON.parse(line, symbolize_names: true)
      rescue JSON::ParserError => e
        raise FormatError, "header is not valid JSON: #{e.message}"
      end
      raise FormatError, "missing JSON object header" unless meta.is_a?(Hash)
      unless meta[:format] == FORMAT
        raise FormatError, "format=#{meta[:format].inspect} (expected #{FORMAT.inspect})"
      end

      size = meta[:event_struct_size]
      if size && size != EVENT_STRUCT_SIZE
        raise FormatError, "event_struct_size=#{size} (expected #{EVENT_STRUCT_SIZE})"
      end

      events = []
      while (rec = io.read(EVENT_STRUCT_SIZE))
        break if rec.bytesize < EVENT_STRUCT_SIZE

        events << unpack_record(rec)
      end
      { meta: meta, events: events }
    end
  end
end
