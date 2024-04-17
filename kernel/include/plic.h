#ifndef __CAFFEINIX_KERNEL_PLIC_H
#define __CAFFEINIX_KERNEL_PLIC_H

#include <typedefs.h>
#include <mem_layout.h>

void plic_init(void);
void plic_init_hart(void);
int plic_claim(void);
void plic_complete(int irq);

#endif