/*
 * @Author: TroyMitchell
 * @Date: 2024-04-23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/arch/riscv/include/vm.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_ARCH_RISCV_VM_H
#define __CAFFEINIX_ARCH_RISCV_VM_H

#include <riscv.h>

/* For kernel */
void kvm_create(void);
void kvm_init(void);
int kvm_mapping_selftest(void);
int kvm_map_mmio(uint64 address, uint64 size);
/* For general */
int vm_map(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm);
void vm_unmap(pagedir_t pgdir, uint64 va, uint64 npages, int do_free);
void vm_unmap_range(pagedir_t pgdir, uint64 va, uint64 size);
pte_t *PTE(pagedir_t pgdir, uint64 va, int flag);
uint64 va2pa(pagedir_t pgdir, uint64 va);
uint64 kvm_va2pa(uint64 va);
int vm_mapped(pagedir_t pgdir, uint64 va);
int vm_alloc_range(pagedir_t pgdir, uint64 start, uint64 end, int eperm);
int vm_alloc_load_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions);
int vm_alloc_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions);
int vm_protect_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			  int permissions);
uint64 vm_user_pa(pagedir_t pgdir, uint64 va);
uint64 vm_alloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz, int eperm);
uint64 vm_dealloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz);
void vm_clear(pagedir_t pgdir, uint64 va);
int vm_copy(pagedir_t old, pagedir_t new);
void vm_free_user(pagedir_t pgdir);
/* For page-table */
pagedir_t pagedir_alloc(void);
void pagedir_free(pagedir_t pgdir);

int copyout(pagedir_t pgdir, uint64 dstva, char* src, uint64 len);
int copyin(pagedir_t pgdir, char* dst, uint64 srcva, uint64 len);
int copyinstr(pagedir_t pgdir, char *dst, uint64 srcva, uint64 max);
#endif
