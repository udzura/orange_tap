# RedFaucet

RedFaucet hooks Ruby method call/return events with a low overhead native
extension, writes them to a lock-free shared-memory ring buffer, and hands
that memory off (via a Mach send right, over XPC) to a central daemon
process that is expected to convert it into OpenTelemetry spans. The daemon
itself is **out of scope** for this gem; this repository only covers the
in-process hook, the shared-memory data plane, and the XPC control plane.

## Why macOS only

RedFaucet depends directly on two Darwin-only APIs:

- **XPC** (`xpc/xpc.h`) for the control plane: establishing a connection to
  the daemon, handing over the shared-memory handle, sending the symbol
  table, and the synchronous `stop_session` request/reply.
- **Mach virtual memory** (`mach/mach_vm.h`, `mach_vm_allocate`/
  `mach_vm_deallocate`) to allocate the ring buffer's backing memory in a way
  that can be boxed into an XPC shared-memory object (`xpc_shmem_create`).

Neither has a portable equivalent, so `extconf.rb` aborts immediately on any
non-Darwin `RUBY_PLATFORM`. XPC, Mach VM and libdispatch (GCD) are all
re-exported by `libSystem` on macOS, so no extra `-framework`/`-l` linker
flag is required beyond what mkmf already sets up -- only their headers need
to be present (Xcode Command Line Tools).

## Shared memory: why Mach send rights (方式B), not a named `shm_open` (方式A)

Two ways to hand the ring buffer's memory to another process were
considered:

- **方式A (name-based)**: `shm_open("/some-name", ...)`, and pass just the
  name string to the daemon over XPC. Simple, but the name is a
  discoverable, global identifier -- any other process owned by the same
  user that guesses (or enumerates) the name can `shm_open` it too, with no
  way to restrict access to "only the daemon we're talking to."
- **方式B (send-right based, what this gem does)**: `mach_vm_allocate` a
  region, box it with `xpc_shmem_create`, and put that XPC object directly
  into the `hello` message. What's transferred is a **Mach send right** to
  that specific VM region, delivered only to whichever process is on the
  other end of *this* XPC connection. There is no name for anyone else to
  guess; a third process has no way to obtain access without also being
  handed the same XPC object.

The trade-off is that 方式B has no discovery mechanism: the daemon can only
learn about a traced process's shared memory by the process actively
connecting to it and sending `hello`, never the other way around.

### Reconnection after a daemon restart

`xpc_connection_create_from_endpoint`/`create_mach_service` connections are
tied to a specific listener instance. If the daemon process restarts, any
Mach send rights it previously received are gone, and a currently-running
traced process's existing XPC connection becomes invalid (its next sync
request fails with `RedFaucet::XPCError`). **This gem does not attempt
automatic reconnection.** A traced process that needs to keep recording
after a daemon restart must call `RedFaucet.configure` again (which
reallocates the ring buffer and re-sends `hello` + the symbol table over a
fresh connection). This is a deliberate initial-scope limitation, not an
oversight -- see DESIGN.md for the reasoning.

## Ring buffer overflow behavior

The ring buffer never blocks the writer (the event hook). If the daemon
falls behind and a write would land on a slot the reader hasn't consumed
yet, the old event is silently overwritten and `dropped_count` (an
`_Atomic uint64_t` in the shared `ring_buffer_t` header) is incremented.
Nothing else changes -- no exception, no backpressure. Consumers (the daemon,
or `RedFaucet._ring_buffer_stats` in tests) are expected to poll
`dropped_count` and treat a nonzero/increasing value as "this capture has
gaps," rather than relying on every event being present.

## Why the hook never allocates a Ruby object

The hot path (`event_hook` in `ext/red_faucet/red_faucet.c`) runs on every
traced method's CALL/RETURN/C_CALL/C_RETURN, for the entire time a recording
session is active. Its only two responsibilities are: (1) an O(1) check of
whether recording is currently active (`session_active_id`) and whether this
particular `(klass, mid)` pair is one we care about (`probe_table_lookup`,
a plain C `st_table`), and (2) an atomic write into shared memory.

Nothing about that requires a new Ruby object. Concretely:

- The hook is attached via a single, permanent `TracePoint` created with
  `rb_tracepoint_new`/`rb_tracepoint_enable` at load time, not re-created per
  call. Reading `rb_tracearg_event_flag`/`rb_tracearg_defined_class` off of
  it touches already-live VM state, no allocation.
- `probe_table_lookup` filters on the raw `VALUE klass` / `ID mid` the VM
  already had on hand -- no `"Class#method"` String is built to do this
  filtering. (Building that string does happen, once, when `trace_method` is
  called to *register* a probe -- that's setup time, not hot path.)
- `tp.binding`, `tp.parameters`, and anything that would build a
  String/Hash/Array are never called from the hook.
- `TracePoint#enable(target:)` is intentionally not used to scope the hook,
  since it only scopes to a single method/object, not a dynamically-growing
  set; instead there is exactly one always-on TracePoint, and the C-level
  `probe_table` is the only thing `trace_method`/`untrace_method` ever
  mutate.

We initially tried the legacy `rb_add_event_hook`/`rb_add_event_hook2`
callback shape instead of a TracePoint, hoping to avoid even the one
permanent TracePoint object. On the Ruby version this was built against,
that legacy callback only populates `self`/`mid`/`klass` eagerly for
`C_CALL`/`C_RETURN`; `CALL`/`RETURN` came through with `mid=0`/`klass=false`
unless `RUBY_EVENT_HOOK_FLAG_RAW_ARG` is set, and that flag turned out to
report internal/undocumented event codes rather than the public
`RUBY_EVENT_*` bitmask. The TracePoint-based approach above is the one that
is both correct and satisfies the "no allocation" requirement.

Class/method registrations made via `trace_method` are also pinned forever
with `rb_gc_register_mark_object` (both keeping them alive and preventing
the GC compactor from relocating them), so the hot path's `probe_table`
lookup never has to worry about a stale/moved `VALUE`. This intentionally
leaks: a class you've traced once stays alive for the life of the process,
which is an acceptable trade-off given trace targets are expected to be a
small, fixed set decided at startup.

## Raw event format and OTel conversion (reference implementations)

The actual "turn CALL/RETURN events into OTLP spans" logic belongs to the
central daemon, which is out of scope here. That said, this gem ships two
small reference modules -- adapted from
[`vivarium`](https://github.com/udzura/vivarium)'s `RawStore` and
`OtelExporter` (same author) -- that a Ruby-based daemon implementation is
free to reuse as-is:

- `RedFaucet::RawStore` (`lib/red_faucet/raw_store.rb`): packs/unpacks
  `trace_event_t`-shaped records to/from a file that's a single JSON
  metadata line followed by fixed-size (40 byte) binary records, mirroring
  `Vivarium::RawStore`'s "one JSON header + raw struct records" format.
- `RedFaucet::OtelExporter` (`lib/red_faucet/otel_exporter.rb`): the OTLP
  `ResourceSpans` JSON envelope/span-shaping helpers are carried over from
  `Vivarium::OtelExporter` largely as-is (they're generic); `build_spans`
  itself is new, since RedFaucet's event schema (`probe_id`/`call_id`/
  `session_id`) is simpler than Vivarium's eBPF-derived events and is
  reconstructed from a per-thread `call_id` stack instead.

The bundled test suite's in-process mock daemon
(`ext/red_faucet/mock_daemon.c` + `RedFaucet._mock_daemon_build_span_file`)
uses exactly these two modules to answer `stop_session` requests, so they
also double as a runnable example of the intended daemon-side flow.

## XPC protocol (for daemon implementers)

All messages are `xpc_dictionary`s with a `"type"` string key.

| `type` | Direction | When | Keys |
|---|---|---|---|
| `hello` | client → daemon, async | once per connection | `type` (string), `pid` (int64), `started_at_ns` (uint64), `ruby_version` (string), `ring_buffer_capacity` (uint64), `shared_memory` (an `xpc_shmem_create` object covering the ring buffer) |
| `symbol_table` | client → daemon, async | after every `trace_method`/`trace_all_instance_methods` call while connected (and once as part of the initial connection) | `type` (string), `entries` (a nested dictionary: probe_id as a decimal-string key → `"Class#method"` string value) |
| `stop_session` | client → daemon, **sync** (`xpc_connection_send_message_with_reply_sync`) | `RedFaucet#stop` | request: `type` (string), `session_id` (uint64). reply: `file_path` (string, the generated Span JSON's path) **or** `error` (string) |
| `process_exit` | client → daemon, async | `at_exit` | `type` (string), `pid` (int64) |

`probe_id → "Class#method"` names come from the symbol table above; raw
events carry only the numeric `probe_id`, never a string, to keep the hot
path allocation-free (see above).

## Public API

```ruby
RedFaucet.configure(mach_service_name: "com.example.tracer_daemon", ring_buffer_capacity: 65536)
RedFaucet.trace_method(SomeClass, :some_method)
RedFaucet.trace_all_instance_methods(SomeClass)
RedFaucet.untrace_method(SomeClass, :some_method)

# Block form
path = RedFaucet.open do
  # CALL/RETURN in this region become one recording session
end # => path to the generated Span JSON file (String)

# Instance form (what the block form uses internally)
tape = RedFaucet.new
tape.open
# ...
path = tape.stop
```

`trace_method`/`trace_all_instance_methods`/`untrace_method` control **what**
gets hooked (a static, C-level registration); `open`/`stop` control **when**
recording actually happens (a dynamic, per-session on/off switch). Opening a
session while one is already active raises
`RedFaucet::AlreadyRecordingError` -- only one recording session is
supported process-wide at a time. Calling `stop` on an instance that was
never opened (or already stopped) raises `RedFaucet::NotRecordingError`.

## Development

After checking out the repo, run `bin/setup` to install dependencies, then
`bundle exec rake` to build the native extension and run the test suite
(`rake compile` builds `ext/red_faucet` on its own if needed). `bin/console`
gives you an interactive prompt with the gem loaded.

## Contributing

Bug reports and pull requests are welcome on GitHub at
https://github.com/udzura/red_faucet.

## Code of Conduct

Everyone interacting in the RedFaucet project's codebases, issue trackers,
chat rooms and mailing lists is expected to follow the
[code of conduct](https://github.com/udzura/red_faucet/blob/main/CODE_OF_CONDUCT.md).
