#ifndef __CAFFEINIX_ARCH_RISCV_TIMER_H
#define __CAFFEINIX_ARCH_RISCV_TIMER_H

#include <typedefs.h>

void timer_init(void);
void timer_init_hart(void);
void timer_interrupt(void);
uint64 timer_frequency(void);

#endif
