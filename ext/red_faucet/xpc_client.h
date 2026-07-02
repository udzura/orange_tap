#ifndef RED_FAUCET_XPC_CLIENT_H
#define RED_FAUCET_XPC_CLIENT_H

#include <stdint.h>
#include <xpc/xpc.h>

#include "ring_buffer.h"

void xpc_client_init(void);

/* Points subsequent xpc_client_configure() calls at an in-process anonymous
 * XPC listener (see mock_daemon.c) instead of a real launchd mach service.
 * Test-only; production code never calls this. */
void xpc_client_use_endpoint(xpc_endpoint_t endpoint);

/* (Re)connects to `mach_service_name` and sends the "hello" message (pid,
 * start time, Ruby version, a shmem handle covering `ring`/`capacity`) plus
 * the current symbol table. A NULL name leaves the client disconnected --
 * xpc_client_stop_session_sync will then fail with an error message.
 * `ruby_version` is passed in from Ruby (RUBY_VERSION) since that constant
 * isn't exposed as a C macro to extensions in modern Ruby header layouts. */
void xpc_client_configure(const char *mach_service_name, const char *ruby_version,
                           ring_buffer_t *ring, uint64_t capacity);

/* Resends the full symbol table over the existing connection (no-op if not
 * connected). Called after trace_method/trace_all_instance_methods register
 * new probes. */
void xpc_client_resend_symbol_table(void);

/* Synchronous "stop_session" request/reply (xpc_connection_send_message_with_reply_sync).
 * On success returns a malloc'd C string (caller frees it) with the
 * daemon-provided file path. On failure returns NULL and, if error_message is
 * non-NULL, *error_message is set to a malloc'd human-readable reason. */
char *xpc_client_stop_session_sync(uint64_t session_id, char **error_message);

/* Fire-and-forget "process_exit" notification, called from Ruby's at_exit. */
void xpc_client_notify_exit(void);

#endif /* RED_FAUCET_XPC_CLIENT_H */
