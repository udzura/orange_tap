#include "clock.h"

#include <mach/mach_time.h>

static mach_timebase_info_data_t g_timebase;

void
red_faucet_clock_init(void)
{
  mach_timebase_info(&g_timebase);
}

uint64_t
red_faucet_now_ns(void)
{
  uint64_t raw = mach_continuous_time();
  return (uint64_t)(((__uint128_t)raw * g_timebase.numer) / g_timebase.denom);
}
