#ifndef __CAFFEINIX_KERNEL_PLIC_H
#define __CAFFEINIX_KERNEL_PLIC_H

#include <typedefs.h>
#include <mem_layout.h>

void plic_init(void);
void plic_init_hart(void);
void plic_enable(uint32 irq);
void plic_disable(uint32 irq);
int plic_claim(void);
void plic_complete(int irq);

#endif
