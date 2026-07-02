#include "session.h"

#include <stdatomic.h>

static _Atomic uint64_t g_active_session_id = 0;
static _Atomic uint64_t g_next_session_id = 1;
static _Atomic uint64_t g_next_call_id = 1;

#define CALL_STACK_DEPTH 256
static __thread uint64_t g_call_stack[CALL_STACK_DEPTH];
static __thread int g_call_depth = 0;

void
session_init(void)
{
  atomic_init(&g_active_session_id, 0);
  atomic_init(&g_next_session_id, 1);
  atomic_init(&g_next_call_id, 1);
}

uint64_t
session_active_id(void)
{
  return atomic_load_explicit(&g_active_session_id, memory_order_relaxed);
}

uint64_t
session_try_open(void)
{
  uint64_t expected = 0;
  uint64_t new_id = atomic_fetch_add_explicit(&g_next_session_id, 1, memory_order_relaxed);

  if (atomic_compare_exchange_strong_explicit(&g_active_session_id, &expected, new_id,
                                               memory_order_acq_rel, memory_order_relaxed)) {
    return new_id;
  }
  return 0;
}

bool
session_close(uint64_t session_id)
{
  uint64_t expected = session_id;
  return atomic_compare_exchange_strong_explicit(&g_active_session_id, &expected, 0,
                                                  memory_order_acq_rel, memory_order_relaxed);
}

uint64_t
session_call_id_for(trace_event_type_t event_type)
{
  if (event_type == TRACE_EVENT_CALL) {
    uint64_t id = atomic_fetch_add_explicit(&g_next_call_id, 1, memory_order_relaxed);
    if (g_call_depth < CALL_STACK_DEPTH) {
      g_call_stack[g_call_depth++] = id;
    }
    return id;
  }

  if (g_call_depth > 0) {
    return g_call_stack[--g_call_depth];
  }
  /* Unmatched RETURN: either the call stack overflowed (recursion deeper than
   * CALL_STACK_DEPTH) or the hook attached mid-call. 0 is not a valid call_id
   * (counter starts at 1), so downstream consumers can treat it as
   * "unpaired". */
  return 0;
}
