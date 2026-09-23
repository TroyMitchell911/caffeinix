/*
 * Fixed-point system load snapshots; the three values represent 1, 5, and 15
 * minute averages scaled by LOADAVG_FIXED, not percentages.
 */
#ifndef __CAFFEINIX_KERNEL_LOADAVG_H
#define __CAFFEINIX_KERNEL_LOADAVG_H

#include <typedefs.h>

#define LOADAVG_FIXED_SHIFT 11
#define LOADAVG_FIXED       (1U << LOADAVG_FIXED_SHIFT)

void loadavg_init(void);
void loadavg_get(uint32 average[3]);

#endif
