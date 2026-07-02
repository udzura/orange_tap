#ifndef RED_FAUCET_SESSION_H
#define RED_FAUCET_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "ring_buffer.h"

void session_init(void);

/* Currently active session_id (0 = no recording in progress). A single
 * relaxed atomic load -- safe and cheap to call from the hot path on every
 * event, which is how open-region-only recording is enforced. */
uint64_t session_active_id(void);

/* Tries to start a new session: CAS the global active id 0 -> new id. Returns
 * the new session_id on success, or 0 if a session was already active (the
 * Ruby-level wrapper turns that into RedFaucet::AlreadyRecordingError). Only
 * one session may be active process-wide at a time (see DESIGN.md's decision
 * against nested/concurrent sessions in the initial scope). */
uint64_t session_try_open(void);

/* Detaches `session_id` as the active session (sets it back to 0). Returns
 * false if `session_id` was not the currently active one. */
bool session_close(uint64_t session_id);

/* Thread-local CALL/RETURN pairing: a small fixed-depth per-OS-thread stack
 * of call_ids plus a global atomic counter. No malloc, no locks -- safe for
 * the hot path even with many concurrent Ruby threads. */
uint64_t session_call_id_for(trace_event_type_t event_type);

#endif /* RED_FAUCET_SESSION_H */
