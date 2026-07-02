#ifndef RED_FAUCET_PROBE_TABLE_H
#define RED_FAUCET_PROBE_TABLE_H

#include <ruby.h>
#include <stdbool.h>
#include <stdint.h>

void probe_table_init(void);

/* Hot-path lookup: a single st_table probe, no Ruby object allocation. Called
 * from event_hook on every CALL/RETURN/C_CALL/C_RETURN while a session is
 * active, so it must stay allocation-free. */
bool probe_table_lookup(VALUE klass, ID mid, uint64_t *probe_id_out);

/* Setup-path only (called from RedFaucet.trace_method etc, never from the
 * hook). Idempotent: registering the same (klass, mid) twice returns the
 * existing probe_id. */
uint64_t probe_table_register(VALUE klass, ID mid, const char *qualified_name);
bool probe_table_unregister(VALUE klass, ID mid);

const char *probe_table_name_for(uint64_t probe_id);
uint64_t probe_table_max_probe_id(void);

/* Builds a Ruby Hash {probe_id (Integer) => "Class#method" (String)} for the
 * XPC symbol_table message. Setup/control-plane path only. */
VALUE probe_table_symbol_hash(void);

#endif /* RED_FAUCET_PROBE_TABLE_H */
