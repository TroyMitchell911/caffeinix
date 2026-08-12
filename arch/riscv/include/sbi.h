#ifndef __CAFFEINIX_ARCH_RISCV_SBI_H
#define __CAFFEINIX_ARCH_RISCV_SBI_H

#include <typedefs.h>

void sbi_init(void);
void sbi_report(void);
int64 sbi_set_timer(uint64 deadline);

#endif
