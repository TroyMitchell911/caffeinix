#ifndef __CAFFEINIX_ARCH_RISCV_TRAP_H
#define __CAFFEINIX_ARCH_RISCV_TRAP_H

#include <riscv.h>

void trap_init(void);
void trap_init_lock(void);

#endif