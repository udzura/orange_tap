#ifndef RED_FAUCET_CLOCK_H
#define RED_FAUCET_CLOCK_H

#include <stdint.h>

/* Caches mach_timebase_info() once at load time (setup path, not the hot
 * path) so red_faucet_now_ns() below never has to look it up. */
void red_faucet_clock_init(void);

/* mach_continuous_time() converted to nanoseconds. mach_continuous_time() is
 * a cheap userspace commpage read on Apple platforms (not a syscall), and the
 * numer/denom multiply/divide here is branch- and lock-free, so this is safe
 * to call from the event_hook hot path as required by the design. */
uint64_t red_faucet_now_ns(void);

#endif /* RED_FAUCET_CLOCK_H */
