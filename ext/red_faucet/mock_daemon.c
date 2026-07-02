#include "mock_daemon.h"

#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "xpc_client.h"

static xpc_connection_t g_listener = NULL;
static dispatch_queue_t g_queue = NULL;
static int g_req_fds[2] = { -1, -1 };  /* [0]=read (Ruby side), [1]=write (here) */
static int g_resp_fds[2] = { -1, -1 }; /* [0]=read (here), [1]=write (Ruby side) */

/* Runs on a libdispatch worker thread. Talks to the Ruby bridge thread purely
 * over POSIX pipes -- no Ruby API calls here, so it is safe regardless of
 * which (non-Ruby) thread libdispatch happened to run this on. The Ruby side
 * (RedFaucet._mock_daemon_bridge_loop) must write with sync=true: a buffered
 * write that never actually reaches the pipe would hang these reads
 * forever. */
static void
bridge_stop_session(uint64_t session_id, char **out_path, char **out_error)
{
  *out_path = NULL;
  *out_error = NULL;

  if (g_req_fds[1] < 0) {
    *out_error = strdup("mock daemon bridge is not running");
    return;
  }

  if (write(g_req_fds[1], &session_id, sizeof(session_id)) != (ssize_t)sizeof(session_id)) {
    *out_error = strdup("mock daemon bridge: failed to write request");
    return;
  }

  uint8_t ok = 0;
  if (read(g_resp_fds[0], &ok, sizeof(ok)) != (ssize_t)sizeof(ok)) {
    *out_error = strdup("mock daemon bridge: failed to read status byte");
    return;
  }

  uint32_t len = 0;
  if (read(g_resp_fds[0], &len, sizeof(len)) != (ssize_t)sizeof(len)) {
    *out_error = strdup("mock daemon bridge: failed to read length");
    return;
  }

  char *buf = malloc((size_t)len + 1);
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(g_resp_fds[0], buf + total, len - total);
    if (n <= 0) {
      free(buf);
      *out_error = strdup("mock daemon bridge: short read");
      return;
    }
    total += (size_t)n;
  }
  buf[len] = '\0';

  if (ok) {
    *out_path = buf;
  } else {
    *out_error = buf;
  }
}

static void
handle_peer_message(xpc_connection_t remote, xpc_object_t message)
{
  const char *type = xpc_dictionary_get_string(message, "type");
  if (type == NULL || strcmp(type, "stop_session") != 0) {
    /* "hello" / "symbol_table" are accepted silently: this mock re-reads the
     * shared ring buffer directly by session_id when asked to stop, so it
     * doesn't need to track the process metadata or symbol table it was
     * sent. */
    return;
  }

  uint64_t session_id = xpc_dictionary_get_uint64(message, "session_id");
  char *path = NULL;
  char *error = NULL;
  bridge_stop_session(session_id, &path, &error);

  xpc_object_t reply = xpc_dictionary_create_reply(message);
  if (path != NULL) {
    xpc_dictionary_set_string(reply, "file_path", path);
  } else {
    xpc_dictionary_set_string(reply, "error", error != NULL ? error : "mock daemon failed");
  }
  xpc_connection_send_message(remote, reply);
  xpc_release(reply);
  free(path);
  free(error);
}

void
mock_daemon_start(int *out_request_read_fd, int *out_response_write_fd)
{
  int req_pipe[2];
  int resp_pipe[2];
  if (pipe(req_pipe) != 0 || pipe(resp_pipe) != 0) {
    *out_request_read_fd = -1;
    *out_response_write_fd = -1;
    return;
  }
  g_req_fds[0] = req_pipe[0];
  g_req_fds[1] = req_pipe[1];
  g_resp_fds[0] = resp_pipe[0];
  g_resp_fds[1] = resp_pipe[1];

  if (g_queue == NULL) {
    g_queue = dispatch_queue_create("red_faucet.mock_daemon", DISPATCH_QUEUE_SERIAL);
  }

  g_listener = xpc_connection_create(NULL, g_queue);
  xpc_connection_set_event_handler(g_listener, ^(xpc_object_t event) {
    if (xpc_get_type(event) != XPC_TYPE_CONNECTION) {
      return;
    }
    xpc_connection_t peer = (xpc_connection_t)event;
    xpc_connection_set_event_handler(peer, ^(xpc_object_t peer_event) {
      if (xpc_get_type(peer_event) == XPC_TYPE_DICTIONARY) {
        handle_peer_message(peer, peer_event);
      }
    });
    xpc_connection_resume(peer);
  });
  xpc_connection_resume(g_listener);

  xpc_endpoint_t endpoint = xpc_endpoint_create(g_listener);
  xpc_client_use_endpoint(endpoint);
  xpc_release(endpoint);

  *out_request_read_fd = g_req_fds[0];
  *out_response_write_fd = g_resp_fds[1];
}
