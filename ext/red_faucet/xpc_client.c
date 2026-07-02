#include "xpc_client.h"

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clock.h"
#include "probe_table.h"

/* Sentinel mach_service_name recognized by create_connection() below: instead
 * of looking the name up via launchd, connect through the anonymous
 * in-process listener set up by xpc_client_use_endpoint (see mock_daemon.c).
 * Only ever passed in from test_helper.rb. */
#define MOCK_SENTINEL "__test_mock__"

static dispatch_queue_t g_queue = NULL;
static xpc_connection_t g_peer = NULL;
static xpc_endpoint_t g_mock_endpoint = NULL;

static ring_buffer_t *g_ring = NULL;
static uint64_t g_capacity = 0;
static char *g_ruby_version = NULL;

static void
teardown_connection(void)
{
  if (g_peer != NULL) {
    xpc_connection_cancel(g_peer);
    xpc_release(g_peer);
    g_peer = NULL;
  }
}

static xpc_connection_t
create_connection(const char *mach_service_name)
{
  if (mach_service_name == NULL) {
    return NULL;
  }
  if (strcmp(mach_service_name, MOCK_SENTINEL) == 0 && g_mock_endpoint != NULL) {
    return xpc_connection_create_from_endpoint(g_mock_endpoint);
  }
  return xpc_connection_create_mach_service(mach_service_name, g_queue, 0);
}

static void
send_hello(void)
{
  if (g_peer == NULL || g_ring == NULL) {
    return;
  }

  xpc_object_t shm = xpc_shmem_create((void *)g_ring, (size_t)ring_buffer_shared_size(g_capacity));
  xpc_object_t dict = xpc_dictionary_create(NULL, NULL, 0);
  xpc_dictionary_set_string(dict, "type", "hello");
  xpc_dictionary_set_int64(dict, "pid", (int64_t)getpid());
  xpc_dictionary_set_uint64(dict, "started_at_ns", red_faucet_now_ns());
  xpc_dictionary_set_string(dict, "ruby_version", g_ruby_version ? g_ruby_version : "");
  xpc_dictionary_set_uint64(dict, "ring_buffer_capacity", g_capacity);
  xpc_dictionary_set_value(dict, "shared_memory", shm);

  xpc_connection_send_message(g_peer, dict);

  xpc_release(shm);
  xpc_release(dict);
}

static void
send_symbol_table(void)
{
  if (g_peer == NULL) {
    return;
  }

  xpc_object_t entries = xpc_dictionary_create(NULL, NULL, 0);
  uint64_t max_id = probe_table_max_probe_id();
  for (uint64_t i = 1; i <= max_id; i++) {
    const char *name = probe_table_name_for(i);
    if (name == NULL) {
      continue;
    }
    char key[32];
    snprintf(key, sizeof(key), "%llu", (unsigned long long)i);
    xpc_dictionary_set_string(entries, key, name);
  }

  xpc_object_t dict = xpc_dictionary_create(NULL, NULL, 0);
  xpc_dictionary_set_string(dict, "type", "symbol_table");
  xpc_dictionary_set_value(dict, "entries", entries);

  xpc_connection_send_message(g_peer, dict);

  xpc_release(entries);
  xpc_release(dict);
}

void
xpc_client_init(void)
{
  if (g_queue == NULL) {
    g_queue = dispatch_queue_create("red_faucet.xpc", DISPATCH_QUEUE_SERIAL);
  }
}

void
xpc_client_use_endpoint(xpc_endpoint_t endpoint)
{
  if (g_mock_endpoint != NULL) {
    xpc_release(g_mock_endpoint);
    g_mock_endpoint = NULL;
  }
  if (endpoint != NULL) {
    xpc_retain(endpoint);
    g_mock_endpoint = endpoint;
  }
}

void
xpc_client_configure(const char *mach_service_name, const char *ruby_version,
                      ring_buffer_t *ring, uint64_t capacity)
{
  xpc_client_init();
  teardown_connection();

  free(g_ruby_version);
  g_ruby_version = ruby_version ? strdup(ruby_version) : NULL;
  g_ring = ring;
  g_capacity = capacity;

  g_peer = create_connection(mach_service_name);
  if (g_peer == NULL) {
    return;
  }

  /* Connection state changes (interruption/invalidation) surface here. There
   * is nothing actionable to do at this layer for an unsolicited event --
   * xpc_client_stop_session_sync surfaces a clear error the next time it's
   * used against a dead peer (see DESIGN.md's note that automatic
   * reconnection after a daemon restart is out of scope for this gem). */
  xpc_connection_set_event_handler(g_peer, ^(xpc_object_t event) {
    (void)event;
  });
  xpc_connection_resume(g_peer);

  send_hello();
  send_symbol_table();
}

void
xpc_client_resend_symbol_table(void)
{
  send_symbol_table();
}

char *
xpc_client_stop_session_sync(uint64_t session_id, char **error_message)
{
  if (g_peer == NULL) {
    if (error_message != NULL) {
      *error_message = strdup(
          "RedFaucet is not connected to a daemon (mach_service_name was not "
          "configured, or the connection could not be established)");
    }
    return NULL;
  }

  xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
  xpc_dictionary_set_string(msg, "type", "stop_session");
  xpc_dictionary_set_uint64(msg, "session_id", session_id);

  xpc_object_t reply = xpc_connection_send_message_with_reply_sync(g_peer, msg);
  xpc_release(msg);

  if (reply == NULL) {
    if (error_message != NULL) {
      *error_message = strdup("no reply received from daemon");
    }
    return NULL;
  }

  if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
    const char *desc = xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION);
    if (error_message != NULL) {
      *error_message = strdup(desc != NULL ? desc : "XPC connection error");
    }
    xpc_release(reply);
    return NULL;
  }

  const char *daemon_error = xpc_dictionary_get_string(reply, "error");
  if (daemon_error != NULL) {
    if (error_message != NULL) {
      *error_message = strdup(daemon_error);
    }
    xpc_release(reply);
    return NULL;
  }

  const char *path = xpc_dictionary_get_string(reply, "file_path");
  if (path == NULL) {
    if (error_message != NULL) {
      *error_message = strdup("daemon reply did not include a file_path");
    }
    xpc_release(reply);
    return NULL;
  }

  char *result = strdup(path);
  xpc_release(reply);
  return result;
}

void
xpc_client_notify_exit(void)
{
  if (g_peer == NULL) {
    return;
  }

  xpc_object_t dict = xpc_dictionary_create(NULL, NULL, 0);
  xpc_dictionary_set_string(dict, "type", "process_exit");
  xpc_dictionary_set_int64(dict, "pid", (int64_t)getpid());
  xpc_connection_send_message(g_peer, dict);
  xpc_release(dict);

  teardown_connection();
}
