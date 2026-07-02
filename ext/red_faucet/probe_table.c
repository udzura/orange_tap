#include "probe_table.h"

#include <ruby/st.h>
#include <stdlib.h>
#include <string.h>

/* Key type for the probe table. We key on the raw (defining class, method id)
 * pair rather than on a Ruby-level "Class#method" string so the hot-path
 * lookup never has to build a String: event_hook already has `klass`/`mid`
 * available directly from rb_add_event_hook's callback arguments. */
typedef struct {
  VALUE klass;
  ID mid;
} probe_key_t;

static st_table *g_probe_table = NULL;
static uint64_t g_next_probe_id = 1;

/* probe_id -> "Class#method" string, indexed by (probe_id - 1). probe_id is a
 * small sequential counter assigned only at registration time (never on the
 * hot path), so a plain growable array is enough -- no hashing needed here. */
static char **g_names = NULL;
static uint64_t g_names_capacity = 0;

static int
probe_key_cmp(st_data_t a, st_data_t b)
{
  probe_key_t *ka = (probe_key_t *)a;
  probe_key_t *kb = (probe_key_t *)b;
  return !(ka->klass == kb->klass && ka->mid == kb->mid);
}

static st_index_t
probe_key_hash(st_data_t arg)
{
  probe_key_t *k = (probe_key_t *)arg;
  st_index_t h = st_hash_start((st_index_t)k->klass);
  h = st_hash_uint(h, (st_index_t)k->mid);
  return h;
}

static const struct st_hash_type probe_key_hash_type = {
  probe_key_cmp,
  probe_key_hash,
};

void
probe_table_init(void)
{
  if (g_probe_table == NULL) {
    g_probe_table = st_init_table(&probe_key_hash_type);
  }
}

bool
probe_table_lookup(VALUE klass, ID mid, uint64_t *probe_id_out)
{
  probe_key_t key = { klass, mid };
  st_data_t value;

  if (st_lookup(g_probe_table, (st_data_t)&key, &value)) {
    *probe_id_out = (uint64_t)value;
    return true;
  }
  return false;
}

static void
grow_names_table(uint64_t probe_id)
{
  if (probe_id <= g_names_capacity) {
    return;
  }

  uint64_t new_cap = g_names_capacity == 0 ? 64 : g_names_capacity * 2;
  while (new_cap < probe_id) {
    new_cap *= 2;
  }

  g_names = realloc(g_names, new_cap * sizeof(char *));
  memset(g_names + g_names_capacity, 0, (new_cap - g_names_capacity) * sizeof(char *));
  g_names_capacity = new_cap;
}

uint64_t
probe_table_register(VALUE klass, ID mid, const char *qualified_name)
{
  uint64_t existing;
  if (probe_table_lookup(klass, mid, &existing)) {
    return existing;
  }

  probe_key_t *key = malloc(sizeof(probe_key_t));
  key->klass = klass;
  key->mid = mid;

  uint64_t probe_id = g_next_probe_id++;
  st_insert(g_probe_table, (st_data_t)key, (st_data_t)probe_id);

  /* Registered classes/method ids are pinned permanently: rb_gc_register_mark_object
   * both keeps them alive and prevents the GC compactor from moving them, so
   * the raw VALUE/ID stored as our st_table key stays valid forever without
   * needing a custom GC mark/compact callback wired into the hot path. This
   * intentionally leaks (a class you traced once stays alive for the life of
   * the process), which is an acceptable trade-off since trace targets are
   * expected to be a small, fixed set decided at startup. */
  rb_gc_register_mark_object(klass);
  rb_gc_register_mark_object(ID2SYM(mid));

  grow_names_table(probe_id);
  g_names[probe_id - 1] = strdup(qualified_name);

  return probe_id;
}

bool
probe_table_unregister(VALUE klass, ID mid)
{
  probe_key_t key = { klass, mid };
  st_data_t k = (st_data_t)&key;
  st_data_t v;

  if (!st_delete(g_probe_table, &k, &v)) {
    return false;
  }
  /* st_delete rewrote `k` to the original (malloc'd) stored key on success. */
  free((probe_key_t *)k);
  return true;
}

const char *
probe_table_name_for(uint64_t probe_id)
{
  if (probe_id == 0 || probe_id > g_names_capacity) {
    return NULL;
  }
  return g_names[probe_id - 1];
}

uint64_t
probe_table_max_probe_id(void)
{
  return g_next_probe_id - 1;
}

VALUE
probe_table_symbol_hash(void)
{
  VALUE hash = rb_hash_new();
  uint64_t max_id = probe_table_max_probe_id();

  for (uint64_t i = 1; i <= max_id; i++) {
    const char *name = probe_table_name_for(i);
    if (name != NULL) {
      rb_hash_aset(hash, ULL2NUM(i), rb_str_new_cstr(name));
    }
  }
  return hash;
}
