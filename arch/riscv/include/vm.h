#ifndef __CAFFEINIX_ARCH_RISCV_VM_H
#define __CAFFEINIX_ARCH_RISCV_VM_H

#include <riscv.h>

void vm_create(void);
void vm_init(void);
void vmmap(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm);

#endif