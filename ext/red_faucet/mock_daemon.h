#ifndef RED_FAUCET_MOCK_DAEMON_H
#define RED_FAUCET_MOCK_DAEMON_H

/* Test-only: stands in for the (out of scope) central daemon by creating an
 * in-process anonymous XPC listener and registering its endpoint with
 * xpc_client.c, so xpc_client_configure("__test_mock__", ...) connects to it
 * instead of a real launchd mach service.
 *
 * The XPC message handler for an anonymous listener's peer connections runs
 * on a libdispatch worker thread that Ruby never created. Calling into the
 * Ruby C API from such a thread (e.g. via rb_thread_call_with_gvl) is only
 * documented-safe for threads the VM already knows about (typically a Ruby
 * thread that itself released the GVL via rb_thread_call_without_gvl); doing
 * it from a wholly foreign thread risks a crash. So instead, "stop_session"
 * requests are bridged across a pair of plain POSIX pipes to a real Ruby
 * Thread (see RedFaucet._mock_daemon_bridge_loop / _mock_daemon_build_span_file
 * in lib/red_faucet.rb) that performs the actual OTel conversion using
 * RedFaucet::OtelExporter. The pipe read/write calls themselves touch no
 * Ruby state, so they are safe from any thread regardless of GVL ownership.
 *
 * Returns two file descriptors the Ruby side must use:
 *   *out_request_read_fd   -- read session_id requests (8 raw bytes) here
 *   *out_response_write_fd -- write replies here, wire format:
 *                             [1 byte ok][4 byte LE length][payload bytes]
 *                             ok=1 -> payload is the file path
 *                             ok=0 -> payload is an error message
 */
void mock_daemon_start(int *out_request_read_fd, int *out_response_write_fd);

#endif /* RED_FAUCET_MOCK_DAEMON_H */
