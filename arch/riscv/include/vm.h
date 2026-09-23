/*
 * RISC-V SV39 address-space construction and safe user-memory access.
 *
 * Page-table mutation is serialized by the owning process mmap_lock, except
 * during early kernel setup and while constructing an unpublished pagedir.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#ifndef __CAFFEINIX_ARCH_RISCV_VM_H
#define __CAFFEINIX_ARCH_RISCV_VM_H

#include <riscv.h>

struct vma_set;

/**
 * kvm_create() - Construct the bootstrap kernel page directory.
 *
 * Context: Early boot; may allocate and panics on unrecoverable failure.
 */
void kvm_create(void);
/**
 * kvm_init() - Install the completed kernel page directory on this hart.
 *
 * Context: Local S-mode execution; updates SATP and flushes local translations.
 */
void kvm_init(void);
int kvm_mapping_selftest(void);
int kvm_map_mmio(uint64 address, uint64 size);
/**
 * vm_map() - Install level-zero mappings for a byte range.
 * @pgdir: Destination page directory.
 * @va: Virtual start; it is rounded down to a page boundary.
 * @pa: Page-aligned physical start.
 * @size: Nonzero byte range; its final byte selects the final mapped page.
 * @perm: RISC-V leaf PTE permission bits.
 *
 * Context: Caller serializes @pgdir mutation; may allocate page tables.
 * Partial mappings and intermediate tables remain on allocation failure;
 * callers must unwind them. Address arithmetic must stay below MAXVA.
 * Return: %0, or %-1 when page-table allocation/walk fails. A zero @size or
 * an already-valid destination PTE is a kernel bug and panics.
 */
int vm_map(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm);
/**
 * vm_unmap() - Remove mappings starting at a page-aligned base.
 * @pgdir: Address space.
 * @va: Page-aligned base.
 * @npages: Page count.
 * @do_free: Free mapped leaf pages when nonzero.
 *
 * Context: Caller serializes @pgdir mutation; invalid mappings panic.
 * The range must contain 4 KiB leaves. No TLB flush is performed here;
 * the caller handles invalidation before stale translations can be reused.
 */
void vm_unmap(pagedir_t pgdir, uint64 va, uint64 npages, int do_free);
void vm_unmap_range(pagedir_t pgdir, uint64 va, uint64 size);
/**
 * PTE() - Walk toward a level-zero PTE, stopping at a huge leaf if present
 * @pgdir: Address space.
 * @va: Virtual address.
 * @flag: Allocate when nonzero.
 *
 * Context: Caller serializes @pgdir mutation; may allocate.
 * Return: PTE slot, possibly an upper-level leaf, or NULL if the walk fails.
 * The returned slot need not itself be valid. No page reference is acquired.
 */
pte_t *PTE(pagedir_t pgdir, uint64 va, int flag);
uint64 va2pa(pagedir_t pgdir, uint64 va);
uint64 kvm_va2pa(uint64 va);
/**
 * vm_mapped() - Test whether a virtual address has a valid mapping.
 * @pgdir: Address space.
 * @va: Virtual address.
 *
 * Context: Page tables remain stable; does not sleep.
 * Return: Nonzero if mapped.
 */
int vm_mapped(pagedir_t pgdir, uint64 va);
int vm_alloc_range(pagedir_t pgdir, uint64 start, uint64 end, int eperm);
int vm_alloc_load_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions);
int vm_alloc_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions);
int vm_protect_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			  int permissions, const struct vma_set *vmas);
/**
 * vm_resolve_cow() - Make a copy-on-write page writable for one address space.
 * @pgdir: Owning address space.
 * @va: Faulting user address.
 *
 * Context: Caller holds the process mmap_lock; may allocate.
 *
 * Return: One if already writable or successfully resolved, zero if this is
 * not an eligible COW mapping, or -1 on a bad reference/allocation failure.
 */
int vm_resolve_cow(pagedir_t pgdir, uint64 va);
uint64 vm_user_pa(pagedir_t pgdir, uint64 va);
uint64 vm_alloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz, int eperm);
uint64 vm_dealloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz);
void vm_clear(pagedir_t pgdir, uint64 va);
/**
 * vm_copy() - Clone user mappings into @new using COW where permitted.
 * @old: Parent address space.
 * @new: Empty child address space.
 * @vmas: Parent VMA metadata used to choose COW eligibility.
 *
 * Context: Parent mmap_lock held; child is unpublished. May allocate tables
 * and synchronously invalidate online CPUs' TLBs after removing write access.
 *
 * Return: Zero or -1. On failure, child user leaves are released but its
 * intermediate tables remain; parent COW protection may remain validly set.
 */
int vm_copy(pagedir_t old, pagedir_t new, const struct vma_set *vmas);
uint64 vm_user_resident_pages(pagedir_t pgdir);
void vm_free_user(pagedir_t pgdir);
/**
 * pagedir_alloc() - Allocate a zeroed root page table.
 *
 * Context: Takes allocator spinlocks but does not reclaim or sleep.
 * Return: Root page table or NULL.
 */
pagedir_t pagedir_alloc(void);
void pagedir_free(pagedir_t pgdir);

/**
 * copyout() - Copy kernel bytes into writable user memory.
 * @pgdir: User address space.
 * @dstva: User destination.
 * @src: Kernel source.
 * @len: Byte count.
 *
 * Context: Sleepable current-process context for demand faults; do not hold
 * mmap_lock while invoking the faulting copy path. Other directories must
 * already have the required mappings. Each page is temporarily pinned.
 *
 * Return: Zero on a full copy, -1 on a fault; earlier bytes remain written.
 */
int copyout(pagedir_t pgdir, uint64 dstva, char* src, uint64 len);
int copyout_nofault(pagedir_t pgdir, uint64 dstva, char *src, uint64 len);
int vm_prefault_user_write(pagedir_t pgdir, uint64 address, uint64 length);
int copyin(pagedir_t pgdir, char* dst, uint64 srcva, uint64 len);
int copyinstr(pagedir_t pgdir, char *dst, uint64 srcva, uint64 max);
#endif
