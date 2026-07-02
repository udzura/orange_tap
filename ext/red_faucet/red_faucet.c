#include <pthread.h>
#include <ruby.h>
#include <ruby/debug.h>
#include <ruby/thread.h>

#include "clock.h"
#include "mock_daemon.h"
#include "probe_table.h"
#include "ring_buffer.h"
#include "session.h"
#include "xpc_client.h"

static VALUE rb_cRedFaucet;
static VALUE rb_eError;
static VALUE rb_eAlreadyRecordingError;
static VALUE rb_eNotRecordingError;
static VALUE rb_eXPCError;

static ID id_session_id_ivar;

static ring_buffer_t *g_ring = NULL;
static uint64_t g_ring_capacity = 0;

/* =========================================================================
 * Hot path
 *
 * event_hook is registered exactly once, in Init_red_faucet_ext, on a single
 * TracePoint object created via rb_tracepoint_new() and enabled for the
 * lifetime of the process. This is deliberately NOT plain rb_add_event_hook:
 * on this Ruby version that legacy callback only eagerly populates
 * self/mid/klass for RUBY_EVENT_C_CALL/C_RETURN (where the call site already
 * has them on hand) -- RUBY_EVENT_CALL/RETURN come through with mid=0/klass=
 * Qfalse. rb_add_event_hook2's RUBY_EVENT_HOOK_FLAG_RAW_ARG was tried as a
 * fix but turned out to report internal, non-public event codes instead of
 * the documented RUBY_EVENT_* bitmask, so it was dropped as unreliable.
 *
 * Using a real TracePoint instead still satisfies "never allocate a Ruby
 * object in the hook": rb_tracepoint_new() allocates exactly one TracePoint
 * object at startup, not per event, and the VM passes that SAME `tpval` on
 * every firing. rb_tracearg_from_tracepoint/rb_tracearg_event_flag/
 * rb_tracearg_defined_class read already-live VM state (no allocation).
 * rb_tracearg_method_id returns a Symbol, which for method names is always
 * an already-interned, immortal Symbol -- no fresh object is created. What
 * we deliberately never call is tp.binding/tp.parameters or anything that
 * would build a String/Hash/Array, since those *do* allocate.
 *
 * trace_method/trace_all_instance_methods/untrace_method only ever mutate
 * the C-level probe_table; they never touch the TracePoint again. This is
 * what "What to trace" (probe_table, static) vs "When to record" (session.c,
 * dynamic) decouples: the hook itself is global and permanent, filtering is
 * a plain hash lookup, and TracePoint#enable(target:) is intentionally not
 * used since it can only scope to a single method/object, not a
 * dynamically-registered set.
 * ========================================================================= */

static uint32_t
current_thread_numeric_id(void)
{
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  return (uint32_t)tid;
}

static void
event_hook(VALUE tpval, void *data)
{
  (void)data;

  /* Not recording: bail out before touching anything else. This is the
   * dominant case for the entire lifetime of a process outside of
   * RedFaucet#open/.stop regions, so it must be as cheap as a single relaxed
   * atomic load. */
  uint64_t session_id = session_active_id();
  if (session_id == 0 || g_ring == NULL) {
    return;
  }

  rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval);
  VALUE mid_sym = rb_tracearg_method_id(trace_arg);
  if (NIL_P(mid_sym)) {
    return; /* e.g. top-level frames with no associated method id */
  }
  ID mid = SYM2ID(mid_sym);
  VALUE klass = rb_tracearg_defined_class(trace_arg);

  /* C-level hash table probe only: no *new* Ruby object is created to
   * perform this filter (no String for "Class#method", no Hash/Array --
   * klass and the already-interned mid Symbol are the raw values the VM
   * already had on hand). */
  uint64_t probe_id;
  if (!probe_table_lookup(klass, mid, &probe_id)) {
    return;
  }

  rb_event_flag_t evflag = rb_tracearg_event_flag(trace_arg);
  trace_event_type_t type =
      (evflag & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL)) ? TRACE_EVENT_CALL : TRACE_EVENT_RETURN;

  /* Timestamp via mach_continuous_time() directly (no Process.clock_gettime,
   * no syscall beyond the commpage read mach_continuous_time already does),
   * call_id via a thread-local stack (no malloc, no lock), and finally an
   * atomic write into the shared-memory ring buffer. */
  uint64_t call_id = session_call_id_for(type);
  uint64_t timestamp_ns = red_faucet_now_ns();
  uint32_t thread_id = current_thread_numeric_id();

  ring_buffer_push(g_ring, session_id, probe_id, type, thread_id, call_id, timestamp_ns);
}

/* =========================================================================
 * Static configuration: RedFaucet.configure / trace_method / untrace_method
 * ========================================================================= */

static VALUE
build_qualified_name(VALUE klass, ID mid)
{
  VALUE name = rb_str_dup(rb_class_name(klass));
  rb_str_cat_cstr(name, "#");
  rb_str_append(name, rb_id2str(mid));
  return name;
}

static VALUE
rf_s_configure(VALUE self, VALUE service_name, VALUE capacity_num, VALUE ruby_version)
{
  (void)self;

  uint64_t capacity = NUM2ULL(capacity_num);
  if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
    rb_raise(rb_eArgError, "ring_buffer_capacity must be a power of two, got %llu",
             (unsigned long long)capacity);
  }

  if (g_ring != NULL) {
    ring_buffer_free(g_ring, g_ring_capacity);
    g_ring = NULL;
  }

  g_ring = ring_buffer_allocate(capacity);
  if (g_ring == NULL) {
    rb_raise(rb_eError, "mach_vm_allocate failed while sizing the ring buffer (capacity=%llu)",
             (unsigned long long)capacity);
  }
  g_ring_capacity = capacity;

  const char *name = NIL_P(service_name) ? NULL : StringValueCStr(service_name);
  const char *rv = NIL_P(ruby_version) ? NULL : StringValueCStr(ruby_version);
  xpc_client_configure(name, rv, g_ring, g_ring_capacity);

  return Qnil;
}

static VALUE
rf_s_trace_method(VALUE self, VALUE klass, VALUE mid_sym)
{
  (void)self;
  Check_Type(mid_sym, T_SYMBOL);
  ID mid = SYM2ID(mid_sym);

  VALUE qualified = build_qualified_name(klass, mid);
  uint64_t probe_id = probe_table_register(klass, mid, StringValueCStr(qualified));

  xpc_client_resend_symbol_table();

  return ULL2NUM(probe_id);
}

static VALUE
rf_s_untrace_method(VALUE self, VALUE klass, VALUE mid_sym)
{
  (void)self;
  Check_Type(mid_sym, T_SYMBOL);
  ID mid = SYM2ID(mid_sym);

  return probe_table_unregister(klass, mid) ? Qtrue : Qfalse;
}

/* =========================================================================
 * Dynamic session control: RedFaucet#open / #stop
 * ========================================================================= */

static VALUE
rf_open(VALUE self)
{
  uint64_t session_id = session_try_open();
  if (session_id == 0) {
    rb_raise(rb_eAlreadyRecordingError,
             "a RedFaucet recording session is already active for this process");
  }
  rb_ivar_set(self, id_session_id_ivar, ULL2NUM(session_id));
  return self;
}

struct stop_session_ctx {
  uint64_t session_id;
  char *path;
  char *error;
};

static void *
stop_session_without_gvl(void *arg)
{
  struct stop_session_ctx *ctx = (struct stop_session_ctx *)arg;
  /* Blocking XPC round trip: released the GVL for this (see rf_stop) so
   * other Ruby threads -- notably the test mock daemon's bridge thread --
   * can keep running while we wait for the daemon's reply. */
  ctx->path = xpc_client_stop_session_sync(ctx->session_id, &ctx->error);
  return NULL;
}

static VALUE
rf_stop(VALUE self)
{
  VALUE session_id_val = rb_ivar_get(self, id_session_id_ivar);
  if (NIL_P(session_id_val)) {
    rb_raise(rb_eNotRecordingError, "this RedFaucet instance has no active recording session");
  }
  uint64_t session_id = NUM2ULL(session_id_val);

  /* Detach as the active session immediately, before the (possibly slow)
   * XPC round trip, so events happening concurrently elsewhere are no longer
   * attributed to it. */
  session_close(session_id);
  rb_ivar_set(self, id_session_id_ivar, Qnil);

  struct stop_session_ctx ctx = { session_id, NULL, NULL };
  rb_thread_call_without_gvl(stop_session_without_gvl, &ctx, RUBY_UBF_IO, NULL);

  if (ctx.path == NULL) {
    const char *message = ctx.error != NULL ? ctx.error : "unknown XPC error";
    VALUE exc = rb_exc_new2(rb_eXPCError, message);
    free(ctx.error);
    rb_exc_raise(exc);
  }

  VALUE result = rb_str_new_cstr(ctx.path);
  free(ctx.path);
  return result;
}

/* =========================================================================
 * Test-only bindings (see test/ and lib/red_faucet.rb's
 * _mock_daemon_build_span_file / _enable_test_mock_daemon!)
 * ========================================================================= */

static VALUE
rf_s_active_session_id(VALUE self)
{
  (void)self;
  return ULL2NUM(session_active_id());
}

static VALUE
rf_s_symbol_table(VALUE self)
{
  (void)self;
  return probe_table_symbol_hash();
}

static VALUE
rf_s_ring_buffer_stats(VALUE self)
{
  (void)self;
  if (g_ring == NULL) {
    return Qnil;
  }

  VALUE ary = rb_ary_new_capa(4);
  rb_ary_push(ary, ULL2NUM(atomic_load_explicit(&g_ring->write_index, memory_order_relaxed)));
  rb_ary_push(ary, ULL2NUM(atomic_load_explicit(&g_ring->read_index, memory_order_relaxed)));
  rb_ary_push(ary, ULL2NUM(g_ring->capacity));
  rb_ary_push(ary, ULL2NUM(atomic_load_explicit(&g_ring->dropped_count, memory_order_relaxed)));
  return ary;
}

static VALUE
rf_s_ring_buffer_dump_session(VALUE self, VALUE session_id_num)
{
  (void)self;
  if (g_ring == NULL) {
    return rb_ary_new();
  }

  uint64_t session_id = NUM2ULL(session_id_num);
  trace_event_t *buf = ALLOC_N(trace_event_t, g_ring_capacity);
  uint64_t count = ring_buffer_dump_session(g_ring, session_id, buf, g_ring_capacity);

  VALUE result = rb_ary_new_capa((long)count);
  ID k_ts = rb_intern("timestamp_ns");
  ID k_tid = rb_intern("thread_id");
  ID k_type = rb_intern("event_type");
  ID k_probe = rb_intern("probe_id");
  ID k_call = rb_intern("call_id");
  ID k_session = rb_intern("session_id");
  ID v_call = rb_intern("call");
  ID v_return = rb_intern("return");

  for (uint64_t i = 0; i < count; i++) {
    VALUE h = rb_hash_new();
    rb_hash_aset(h, ID2SYM(k_ts), ULL2NUM(buf[i].timestamp_ns));
    rb_hash_aset(h, ID2SYM(k_tid), UINT2NUM(buf[i].thread_id));
    rb_hash_aset(h, ID2SYM(k_type),
                 ID2SYM(buf[i].event_type == TRACE_EVENT_CALL ? v_call : v_return));
    rb_hash_aset(h, ID2SYM(k_probe), ULL2NUM(buf[i].probe_id));
    rb_hash_aset(h, ID2SYM(k_call), ULL2NUM(buf[i].call_id));
    rb_hash_aset(h, ID2SYM(k_session), ULL2NUM(buf[i].session_id));
    rb_ary_push(result, h);
  }
  xfree(buf);
  return result;
}

static VALUE
rf_s_ring_buffer_push_raw(VALUE self, VALUE session_id, VALUE probe_id, VALUE event_type_sym,
                           VALUE thread_id, VALUE call_id, VALUE timestamp_ns)
{
  (void)self;
  if (g_ring == NULL) {
    rb_raise(rb_eError, "RedFaucet.configure must be called first");
  }
  Check_Type(event_type_sym, T_SYMBOL);
  trace_event_type_t type =
      (SYM2ID(event_type_sym) == rb_intern("call")) ? TRACE_EVENT_CALL : TRACE_EVENT_RETURN;

  ring_buffer_push(g_ring, NUM2ULL(session_id), NUM2ULL(probe_id), type,
                    (uint32_t)NUM2UINT(thread_id), NUM2ULL(call_id), NUM2ULL(timestamp_ns));
  return Qnil;
}

static VALUE
rf_s_start_test_mock_daemon(VALUE self)
{
  (void)self;
  int req_fd = -1;
  int resp_fd = -1;
  mock_daemon_start(&req_fd, &resp_fd);
  if (req_fd < 0) {
    rb_raise(rb_eError, "failed to start the test mock daemon (pipe(2) failed)");
  }

  VALUE ary = rb_ary_new_capa(2);
  rb_ary_push(ary, INT2NUM(req_fd));
  rb_ary_push(ary, INT2NUM(resp_fd));
  return ary;
}

static VALUE
rf_s_notify_exit(VALUE self)
{
  (void)self;
  xpc_client_notify_exit();
  return Qnil;
}

void
Init_red_faucet_ext(void)
{
  probe_table_init();
  session_init();
  red_faucet_clock_init();
  xpc_client_init();

  id_session_id_ivar = rb_intern("@session_id");

  rb_cRedFaucet = rb_define_class("RedFaucet", rb_cObject);
  rb_eError = rb_define_class_under(rb_cRedFaucet, "Error", rb_eStandardError);
  rb_eAlreadyRecordingError =
      rb_define_class_under(rb_cRedFaucet, "AlreadyRecordingError", rb_eError);
  rb_eNotRecordingError = rb_define_class_under(rb_cRedFaucet, "NotRecordingError", rb_eError);
  rb_eXPCError = rb_define_class_under(rb_cRedFaucet, "XPCError", rb_eError);

  rb_define_singleton_method(rb_cRedFaucet, "_configure", rf_s_configure, 3);
  rb_define_singleton_method(rb_cRedFaucet, "_trace_method", rf_s_trace_method, 2);
  rb_define_singleton_method(rb_cRedFaucet, "_untrace_method", rf_s_untrace_method, 2);
  rb_define_singleton_method(rb_cRedFaucet, "_active_session_id", rf_s_active_session_id, 0);
  rb_define_singleton_method(rb_cRedFaucet, "_symbol_table", rf_s_symbol_table, 0);
  rb_define_singleton_method(rb_cRedFaucet, "_ring_buffer_stats", rf_s_ring_buffer_stats, 0);
  rb_define_singleton_method(rb_cRedFaucet, "_ring_buffer_dump_session",
                              rf_s_ring_buffer_dump_session, 1);
  rb_define_singleton_method(rb_cRedFaucet, "_ring_buffer_push_raw", rf_s_ring_buffer_push_raw, 6);
  rb_define_singleton_method(rb_cRedFaucet, "_start_test_mock_daemon",
                              rf_s_start_test_mock_daemon, 0);
  rb_define_singleton_method(rb_cRedFaucet, "_notify_exit", rf_s_notify_exit, 0);

  rb_define_method(rb_cRedFaucet, "open", rf_open, 0);
  rb_define_method(rb_cRedFaucet, "stop", rf_stop, 0);

  /* Created and enabled exactly once, here, for the lifetime of the process.
   * See the big comment above event_hook for why trace_method et al. never
   * touch this TracePoint again, and rb_gc_register_mark_object below for why
   * it's safe to let the local VALUE go out of scope. */
  VALUE tp = rb_tracepoint_new(Qnil,
                                RUBY_EVENT_CALL | RUBY_EVENT_RETURN | RUBY_EVENT_C_CALL |
                                    RUBY_EVENT_C_RETURN,
                                event_hook, NULL);
  rb_gc_register_mark_object(tp);
  rb_tracepoint_enable(tp);
}
