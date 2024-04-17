#ifndef __CAFFEINIX_ARCH_RISCV_VM_H
#define __CAFFEINIX_ARCH_RISCV_VM_H

#include <riscv.h>

void vm_create(void);
void vm_init(void);
int vm_map(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm);
pte_t *PTE(pagedir_t pgdir, uint64 va, int flag);

#endif