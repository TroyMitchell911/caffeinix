#ifndef __CAFFEINIX_KERNEL_KTIME_H
#define __CAFFEINIX_KERNEL_KTIME_H

#include <typedefs.h>

uint64 ktime_get_ticks(void);
uint64 ktime_get_ms(void);
uint64 ktime_get_ns(void);
uint64 ktime_ms_to_ticks(uint64 milliseconds);

#endif
