#ifndef RED_FAUCET_RING_BUFFER_H
#define RED_FAUCET_RING_BUFFER_H

#include <stdatomic.h>
#include <stdint.h>

typedef enum {
  TRACE_EVENT_CALL = 0,
  TRACE_EVENT_RETURN = 1,
} trace_event_type_t;

/* Fixed-size (40 byte), POD event record. This exact layout is mirrored by
 * RedFaucet::RawStore's PACK_FMT ("Q<L<L<Q<Q<Q<") in lib/red_faucet/raw_store.rb,
 * so keep the two in sync if this struct ever changes. */
typedef struct {
  uint64_t timestamp_ns;
  uint32_t thread_id;
  uint32_t event_type; /* trace_event_type_t */
  uint64_t probe_id;
  uint64_t call_id;
  uint64_t session_id; /* 0 = not part of any recording session */
} trace_event_t;

/* Shared-memory ring buffer header. Lives at the start of the mach_vm_allocate'd
 * region; `events` is a flexible array member filling the rest of the region.
 * Writer (the event hook, potentially many OS threads) uses atomic_fetch_add on
 * write_index, so this is effectively MPSC: many producer threads, one
 * (out-of-process) consumer. */
typedef struct {
  _Atomic uint64_t write_index;
  _Atomic uint64_t read_index;
  uint64_t capacity; /* number of event slots; must be a power of two */
  _Atomic uint64_t dropped_count;
  trace_event_t events[];
} ring_buffer_t;

/* Total byte size of the shared memory region needed for `capacity` slots. */
uint64_t ring_buffer_shared_size(uint64_t capacity);

/* Allocates and initializes a ring buffer of `capacity` slots via
 * mach_vm_allocate. `capacity` must already be a power of two (validated by
 * the Ruby-level caller). Returns NULL on allocation failure. */
ring_buffer_t *ring_buffer_allocate(uint64_t capacity);

/* Deallocates a ring buffer previously returned by ring_buffer_allocate. */
void ring_buffer_free(ring_buffer_t *rb, uint64_t capacity);

/* Hot-path write. No malloc, no locks, no syscalls beyond the atomic ops
 * themselves. Safe to call concurrently from multiple OS threads. */
void ring_buffer_push(ring_buffer_t *rb, uint64_t session_id, uint64_t probe_id,
                       trace_event_type_t event_type, uint32_t thread_id,
                       uint64_t call_id, uint64_t timestamp_ns);

/* Non-destructive scan (does not advance read_index) collecting up to
 * out_capacity events belonging to `session_id`, in chronological (write)
 * order. Returns the number of events written into `out`. This is a
 * test/tooling helper (probe_table_test.rb, session_test.rb, the in-process
 * mock daemon) -- a real out-of-process daemon would instead poll and advance
 * read_index itself, which is out of scope for this gem. */
uint64_t ring_buffer_dump_session(ring_buffer_t *rb, uint64_t session_id,
                                   trace_event_t *out, uint64_t out_capacity);

#endif /* RED_FAUCET_RING_BUFFER_H */
