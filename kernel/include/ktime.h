#ifndef __CAFFEINIX_KERNEL_KTIME_H
#define __CAFFEINIX_KERNEL_KTIME_H

#include <typedefs.h>

#define NSEC_PER_SEC 1000000000ULL

void ktime_boot_init(uint64 ticks);
void ktime_set_realtime_ns(uint64 nanoseconds);
uint64 ktime_get_ticks(void);
uint64 ktime_get_ms(void);
uint64 ktime_get_ns(void);
uint64 ktime_get_boot_ns(void);
int ktime_get_realtime_ns(uint64 *nanoseconds);
uint64 ktime_ticks_to_ns(uint64 ticks, uint32 frequency);
uint64 ktime_ms_to_ticks(uint64 milliseconds);
uint64 ktime_ns_to_ticks(uint64 nanoseconds);

#endif
