#include "ring_buffer.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stddef.h>

uint64_t
ring_buffer_shared_size(uint64_t capacity)
{
  return (uint64_t)sizeof(ring_buffer_t) + capacity * (uint64_t)sizeof(trace_event_t);
}

ring_buffer_t *
ring_buffer_allocate(uint64_t capacity)
{
  mach_vm_address_t addr = 0;
  mach_vm_size_t size = (mach_vm_size_t)ring_buffer_shared_size(capacity);

  kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE);
  if (kr != KERN_SUCCESS) {
    return NULL;
  }

  ring_buffer_t *rb = (ring_buffer_t *)(uintptr_t)addr;
  atomic_init(&rb->write_index, 0);
  atomic_init(&rb->read_index, 0);
  rb->capacity = capacity;
  atomic_init(&rb->dropped_count, 0);
  return rb;
}

void
ring_buffer_free(ring_buffer_t *rb, uint64_t capacity)
{
  if (rb == NULL) {
    return;
  }
  mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)rb,
                      (mach_vm_size_t)ring_buffer_shared_size(capacity));
}

/* Hot path: no malloc, no locks, no syscalls other than the atomic ops
 * themselves. Multiple OS threads may call this concurrently (MPSC), which is
 * why write_index is advanced with a single atomic_fetch_add rather than a
 * read-modify-write under a lock. */
void
ring_buffer_push(ring_buffer_t *rb, uint64_t session_id, uint64_t probe_id,
                  trace_event_type_t event_type, uint32_t thread_id,
                  uint64_t call_id, uint64_t timestamp_ns)
{
  uint64_t idx = atomic_fetch_add_explicit(&rb->write_index, 1, memory_order_relaxed);
  uint64_t read_idx = atomic_load_explicit(&rb->read_index, memory_order_relaxed);

  /* The slot we are about to write has not been consumed yet: it is about to
   * be overwritten (buffer wrapped around faster than the reader drains it).
   * Count it, but still overwrite -- we never block the writer. */
  if (idx - read_idx >= rb->capacity) {
    atomic_fetch_add_explicit(&rb->dropped_count, 1, memory_order_relaxed);
  }

  trace_event_t *slot = &rb->events[idx & (rb->capacity - 1)];
  slot->timestamp_ns = timestamp_ns;
  slot->thread_id = thread_id;
  slot->event_type = (uint32_t)event_type;
  slot->probe_id = probe_id;
  slot->call_id = call_id;
  slot->session_id = session_id;
}

uint64_t
ring_buffer_dump_session(ring_buffer_t *rb, uint64_t session_id, trace_event_t *out,
                          uint64_t out_capacity)
{
  uint64_t write_idx = atomic_load_explicit(&rb->write_index, memory_order_acquire);
  uint64_t read_idx = atomic_load_explicit(&rb->read_index, memory_order_relaxed);
  uint64_t available = write_idx - read_idx;
  if (available > rb->capacity) {
    available = rb->capacity; /* older entries have already been overwritten */
  }

  uint64_t start = write_idx - available;
  uint64_t count = 0;
  for (uint64_t i = start; i < write_idx && count < out_capacity; i++) {
    trace_event_t *slot = &rb->events[i & (rb->capacity - 1)];
    if (slot->session_id == session_id) {
      out[count++] = *slot;
    }
  }
  return count;
}
