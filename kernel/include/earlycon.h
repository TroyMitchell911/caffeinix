#ifndef __CAFFEINIX_KERNEL_EARLYCON_H
#define __CAFFEINIX_KERNEL_EARLYCON_H

#include <typedefs.h>

void earlycon_init(void);
void earlycon_putc(int character);
uint64 earlycon_address(void);
uint64 earlycon_size(void);

#endif
