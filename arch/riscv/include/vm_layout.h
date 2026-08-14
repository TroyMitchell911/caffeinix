#ifndef __CAFFEINIX_ARCH_RISCV_VM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_VM_LAYOUT_H

#include <typedefs.h>

#define SV39_LEVEL_MAX 2

uint64 sv39_level_size(unsigned int level);
int sv39_best_map_level(uint64 virtual, uint64 physical, uint64 size);

#endif
