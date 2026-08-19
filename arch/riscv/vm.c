/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-16
 * @FilePath: /caffeinix/arch/riscv/vm.c
 * @Description: This file about all virtual address
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <palloc.h>
#include <mem_layout.h>
#include <vm.h>
#include <vm_layout.h>
#include <mystring.h>
#include <debug.h>
#include <process.h>
#include <printf.h>
#include <cpu.h>
#include <mmap.h>
#include <linux_uapi.h>
#include <scheduler.h>
#include <vma.h>

/* Defination in kernel.ld */
extern char etext[];

extern char trampoline[];

static pagedir_t kernel_pgdir;

static pte_t *vm_walk(pagedir_t pgdir, uint64 va, int allocate,
		      int target_level, int *leaf_level)
{
	int level;
	pte_t *pte;
	pagedir_t next;

        /* The value of va can't be more than MAXVA */
	if (va >= MAXVA || target_level < 0 ||
	    target_level > SV39_LEVEL_MAX)
		return 0;
        /* 
                Starting from a high address,
                retrieve the page table pointed to by the page table entry
        */
	for (level = SV39_LEVEL_MAX; level > target_level; level--) {
                /* Get the page table pointed to by the page table entry */
		pte = &pgdir[PTEX(level, va)];
                /* If the page-table exists */
		if (*pte & PTE_V) {
			if (*pte & (PTE_R | PTE_W | PTE_X)) {
				if (leaf_level)
					*leaf_level = level;
				return pte;
			}
                        /* Store the page-table physical address into pgdir. */
			pgdir = (pagedir_t)PTE2PA(*pte);
                } else {
			if (!allocate || (next = palloc_zero()) == 0)
                                return 0;
                        /* Store the pte */
			*pte = PA2PTE(next) | PTE_V;
			pgdir = next;
                }
        }
	if (leaf_level)
		*leaf_level = target_level;
	return &pgdir[PTEX(target_level, va)];
}

pte_t *PTE(pagedir_t pgdir, uint64 va, int flag)
{
	return vm_walk(pgdir, va, flag, 0, 0);
}

static uint64 user_va2pa(pagedir_t pgdir, uint64 va, int permissions)
{
	pte_t *pte;
	uint64 leaf_size;
	int level;

        if(va >= MAXVA)
                return 0;

	pte = vm_walk(pgdir, va, 0, 0, &level);
        if(pte == 0)
                return 0;
        if((*pte & PTE_V) == 0)
                return 0;
	if ((*pte & PTE_U) == 0 ||
	    (*pte & permissions) != (uint64)permissions)
		return 0;
	leaf_size = sv39_level_size(level);
	return PTE2PA(*pte) +
	       (va & (leaf_size - 1) & ~(PGSIZE - 1));
}

static uint64 user_copy_va2pa_pinned(pagedir_t pgdir, uint64 va,
				     int permissions, int fault,
				     void **pinned_page)
{
	process_t process;
	void *page;
	uint64 physical, validated;
	enum mmap_fault_access access;

	*pinned_page = 0;
	process = cur_proc();
	access = permissions & PTE_W ? MMAP_FAULT_WRITE : MMAP_FAULT_READ;
	for (;;) {
		physical = user_va2pa(pgdir, va, permissions);
		if (!physical) {
			if (!fault || !process || process->pagetable != pgdir ||
			    mmap_handle_fault(process, va, access) != MMAP_FAULT_OK)
				return 0;
			continue;
		}
		page = (void *)PGROUNDDOWN(physical);
		if (palloc_get(page) < 0)
			continue;
		validated = user_va2pa(pgdir, va, permissions);
		if (validated == physical) {
			*pinned_page = page;
			return physical;
		}
		pfree(page);
	}
}

uint64 va2pa(pagedir_t pgdir, uint64 va)
{
	return user_va2pa(pgdir, va, 0);
}

uint64 kvm_va2pa(uint64 va)
{
	pte_t *pte;
	uint64 leaf_size;
	int level;

	if (!kernel_pgdir || va >= MAXVA)
		return 0;
	pte = vm_walk(kernel_pgdir, va, 0, 0, &level);
	if (!pte || !(*pte & PTE_V) ||
	    !(*pte & (PTE_R | PTE_W | PTE_X)))
		return 0;
	leaf_size = sv39_level_size(level);
	return PTE2PA(*pte) + (va & (leaf_size - 1));
}

static int vm_map_leaf(pagedir_t pgdir, uint64 va, uint64 pa, int level,
		       int perm)
{
	uint64 leaf_size = sv39_level_size(level);
	pte_t *pte;
	int found_level;

	if (!leaf_size || va % leaf_size || pa % leaf_size)
		return -1;
	pte = vm_walk(pgdir, va, 1, level, &found_level);
	if (!pte || found_level != level || (*pte & PTE_V))
		return -1;
	*pte = PA2PTE(pa) | PTE_V | perm;
	return 0;
}

static int vm_map_largest(pagedir_t pgdir, uint64 va, uint64 pa,
			  uint64 size, int perm)
{
	uint64 leaf_size;
	int level;

	if (!size || va >= MAXVA || size > MAXVA - va ||
	    pa + size < pa || va % PGSIZE || pa % PGSIZE || size % PGSIZE)
		return -1;
	while (size) {
		level = sv39_best_map_level(va, pa, size);
		leaf_size = sv39_level_size(level);
		if (vm_map_leaf(pgdir, va, pa, level, perm) < 0)
			return -1;
		va += leaf_size;
		pa += leaf_size;
		size -= leaf_size;
	}
	return 0;
}

int vm_map(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm)
{
        uint64 start, end;
        pte_t *pte;
        /* Size can't be 0 */
        if(!size) {
                PANIC("vm_map size");
        }
        /* Aligned downward at 4096 bytes */
        start = PGROUNDDOWN(va);
        end = PGROUNDDOWN(va + size - 1);
        
        for(;;) {
                /* 
                        Obtain the address of the page table entry 
                        with the virtual address in the page table  
                */
                if((pte = PTE(pgdir, start, 1)) == 0)
                        return -1;
                /* The pte can't include the bit PTE_V */
                if(*pte & PTE_V) {
                        PANIC("vm_map remap");
                }
                /* Set the PTE */
                *pte = PA2PTE(pa) | PTE_V | perm;
                if(start == end)
                        break;
                start += PGSIZE;
                pa += PGSIZE;
        }
        return 0;
}

static pagedir_t kernel_pagedir_t_create(void)
{
	uint64 finish, start;
	int i;

        /* Alloc the physical memory for page-table */
	pagedir_t pgdir = (pagedir_t)palloc_zero();

	if (!pgdir)
		PANIC("allocate kernel page table");

        /* Map the trampoline */        
        vm_map(pgdir, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_X | PTE_R);
        /* PLIC */
        vm_map(pgdir, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
        /* Map the text */
        vm_map(pgdir, KERNEL_BASE, KERNEL_BASE,
               (uint64)etext - KERNEL_BASE, PTE_R | PTE_X);
        /* Map the kernel data and static allocations. */
        vm_map(pgdir, (uint64)etext, (uint64)etext,
	       palloc_heap_start() - (uint64)etext,
	       PTE_R | PTE_W);
	/* Map only allocator-owned RAM, leaving reservations inaccessible. */
	for (i = 0; i < palloc_managed_range_count(); i++) {
		if (palloc_managed_range_get(i, &start, &finish) < 0)
			PANIC("memory range");
		if (vm_map_largest(pgdir, start, start, finish - start,
				   PTE_R | PTE_W) < 0)
			PANIC("map physical memory");
	}

        map_kernel_stack(pgdir);
	cpu_map_kernel_stacks(pgdir);
        return pgdir;
}

pagedir_t pagedir_alloc(void)
{
        /* Malloc memory for page-talble */
	pagedir_t pgdir = (pagedir_t)palloc_zero();
        if(!pgdir) {
                return 0;
        }

        return pgdir;
}
/* Recursively free page-table pages. */
void pagedir_free(pagedir_t pgdir)
{
        int i;
        pte_t pte;
        uint64 sub_pgdir_pa;
        /* Any PTE occupies 8 bytes */
        for(i = 0; i < PGSIZE / 8; i ++) {
                pte = pgdir[i];
                if((pte & PTE_V) && ((pte & (PTE_R|PTE_W|PTE_X)) == 0)) {
                        sub_pgdir_pa = PTE2PA(pte);
                        pagedir_free((pagedir_t)sub_pgdir_pa);
                        pgdir[i] = 0;
                } else if((pte & PTE_V)) {
                        /* 
                                We can't accept a PTE has the flag 'V'.
                                Clear V in unmap before freeing a page table.
                        */
                        PANIC("pagedir_free");
                }
        }
        pfree(pgdir);
}

void vm_unmap(pagedir_t pgdir, uint64 va, uint64 npages, int do_free)
{
        uint64 addr;
        pte_t* pte;
        uint64 pa;

        if((va % PGSIZE) != 0)
                panic("uvmunmap: not aligned");

        for(addr = va; addr < va + npages * PGSIZE; addr += PGSIZE) {
                pte = PTE(pgdir, addr, 0);
                if(pte == 0) {
                        PANIC("vm_unmap PTE");
                }
                if((*pte & PTE_V) == 0) {
                        PANIC("vm_unmap not mapped");
                }
                /* Check if the PTE points a page-table */
                if((*pte & 0x3ff) == PTE_V) {
                        PANIC("vm_unmap not a leaf");
                }
                if(do_free) {
                        pa = PTE2PA(*pte);
                        pfree((void*)pa);
                }
                *pte = 0;
        }
}

int vm_mapped(pagedir_t pgdir, uint64 va)
{
	pte_t *pte = PTE(pgdir, va, 0);

	return pte && (*pte & PTE_V) && (*pte & (PTE_R | PTE_W | PTE_X));
}

void vm_unmap_range(pagedir_t pgdir, uint64 va, uint64 size)
{
	uint64 addr, end, pa;
	pte_t *pte;

	if (!size)
		return;
	addr = PGROUNDDOWN(va);
	end = PGROUNDUP(va + size);
	for (; addr < end; addr += PGSIZE) {
		pte = PTE(pgdir, addr, 0);
		if (!pte || !(*pte & PTE_V) ||
		    !(*pte & (PTE_R | PTE_W | PTE_X)))
			continue;
		pa = PTE2PA(*pte);
		*pte = 0;
		pfree((void *)pa);
	}
	cpu_tlb_flush_all();
}

/* Free page-table from oldsz to newsz */
uint64 vm_dealloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz)
{
        if(newsz >= oldsz)
                return oldsz;

        if(PGROUNDUP(newsz) < PGROUNDUP(oldsz))
		vm_unmap_range(pgdir, PGROUNDUP(newsz),
		               PGROUNDUP(oldsz) - PGROUNDUP(newsz));
        return newsz;
}


uint64 vm_alloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz, int eperm)
{
        if(newsz <= oldsz)
                return oldsz;

        if (vm_alloc_range(pgdir, PGROUNDUP(oldsz), PGROUNDUP(newsz),
			   eperm) < 0)
		return 0;
        return newsz;
}

int vm_alloc_range(pagedir_t pgdir, uint64 start, uint64 end, int eperm)
{
	return vm_alloc_user_range(pgdir, start, end,
				   PTE_R | PTE_U | eperm);
}

int vm_alloc_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions)
{
	uint64 addr;
	void *mem;

	if (start > end || start % PGSIZE || end % PGSIZE || end > MAXVA)
		return -1;
	if (!(permissions & (PTE_R | PTE_W | PTE_X)) ||
	    ((permissions & PTE_W) && !(permissions & PTE_R)))
		return -1;
	for (addr = start; addr < end; addr += PGSIZE) {
		if (vm_mapped(pgdir, addr))
			return -1;
	}
	for (addr = start; addr < end; addr += PGSIZE) {
		mem = palloc_zero();
		if (!mem)
			goto fail;
		if (vm_map(pgdir, addr, (uint64)mem, PGSIZE,
			   permissions | PTE_SW_USER) < 0) {
			pfree(mem);
			goto fail;
		}
	}
	sfence_vma();
	return 0;

fail:
	vm_unmap_range(pgdir, start, addr - start);
	return -1;
}

int vm_alloc_load_range(pagedir_t pgdir, uint64 start, uint64 end,
			int permissions)
{
	pte_t *pte;

	if (start > end || start % PGSIZE || end % PGSIZE || end > MAXVA ||
	    !(permissions & (PTE_R | PTE_W | PTE_X)) ||
	    ((permissions & PTE_W) && !(permissions & PTE_R)))
		return -1;
	if (start < end && vm_mapped(pgdir, start)) {
		pte = PTE(pgdir, start, 0);
		if (!pte || !(*pte & PTE_SW_USER))
			return -1;
		if (permissions & PTE_U) {
			if (!(*pte & PTE_U))
				*pte &= ~(PTE_R | PTE_W | PTE_X);
			*pte |= permissions;
		}
		start += PGSIZE;
		sfence_vma();
	}
	return vm_alloc_user_range(pgdir, start, end, permissions);
}

uint64 vm_user_pa(pagedir_t pgdir, uint64 va)
{
	pte_t *pte;

	if (va >= MAXVA)
		return 0;
	pte = PTE(pgdir, va, 0);
	if (!pte || !(*pte & PTE_V) || !(*pte & PTE_SW_USER) ||
	    !(*pte & (PTE_R | PTE_W | PTE_X)))
		return 0;
	return PTE2PA(*pte) + (va & (PGSIZE - 1));
}

int vm_protect_user_range(pagedir_t pgdir, uint64 start, uint64 end,
			  int permissions, const struct vma_set *vmas)
{
	uint64 addr;
	pte_t *pte;

	if (start >= end || start % PGSIZE || end % PGSIZE || end > MAXVA ||
	    !(permissions & (PTE_R | PTE_W | PTE_X)) ||
	    ((permissions & PTE_W) && !(permissions & PTE_R)))
		return -1;
	for (addr = start; addr < end; addr += PGSIZE) {
		pte = PTE(pgdir, addr, 0);
		if (!pte || !(*pte & PTE_V))
			continue;
		if (!(*pte & PTE_SW_USER) ||
		    !(*pte & (PTE_R | PTE_W | PTE_X)))
			return -1;
	}
	for (addr = start; addr < end; addr += PGSIZE) {
		const struct vm_area *area;
		int page_permissions = permissions;
		pte_t software;

		pte = PTE(pgdir, addr, 0);
		if (!pte || !(*pte & PTE_V))
			continue;
		area = vma_find(vmas, addr);
		if (!area)
			return -1;
		software = *pte & PTE_SW_COW;
		if (permissions & PTE_W) {
			if ((area->flags & 0xf) == LINUX_MAP_PRIVATE &&
			    palloc_refcount((void *)PTE2PA(*pte)) > 1) {
				page_permissions &= ~PTE_W;
				software = PTE_SW_COW;
			} else {
				software = 0;
			}
		} else if ((area->flags & 0xf) != LINUX_MAP_PRIVATE) {
			software = 0;
		}
		*pte = PA2PTE(PTE2PA(*pte)) | PTE_V | PTE_SW_USER |
		       software | (*pte & (PTE_A | PTE_D)) |
		       page_permissions;
	}
	cpu_tlb_flush_all();
	return 0;
}

int vm_resolve_cow(pagedir_t pgdir, uint64 va)
{
	uint32 references;
	uint64 old_pa;
	void *page;
	pte_t old, *pte;

	pte = PTE(pgdir, PGROUNDDOWN(va), 0);
	if (!pte || !(*pte & PTE_V) || !(*pte & PTE_SW_USER))
		return 0;
	/* Another thread may have resolved this write fault first. */
	if (!(*pte & PTE_SW_COW))
		return *pte & PTE_W ? 1 : 0;
	old = *pte;
	old_pa = PTE2PA(old);
	references = palloc_refcount((void *)old_pa);
	if (!references)
		return -1;
	if (references == 1) {
		*pte = (old | PTE_W) & ~PTE_SW_COW;
		cpu_tlb_flush_all();
		return 1;
	}
	page = alloc_pages(0, 0);
	if (!page)
		return -1;
	memmove(page, (void *)old_pa, PGSIZE);
	*pte = PA2PTE(page) | (old & 0x3ff) | PTE_W;
	*pte &= ~PTE_SW_COW;
	pfree((void *)old_pa);
	cpu_tlb_flush_all();
	return 1;
}

void vm_clear(pagedir_t pgdir, uint64 va)
{
        pte_t* pte = PTE(pgdir, va, 0);
        if(!pte) {
                PANIC("vm_clear");
        } else {
                *pte &= ~PTE_U;
        }
}

static int vm_copy_walk(pagedir_t old, pagedir_t new, int level,
			uint64 base, const struct vma_set *vmas,
			int *parent_changed)
{
	const struct vm_area *area;
	uint64 pa, va;
	pte_t child_pte, pte;
	int i;

	for (i = 0; i < PGSIZE / sizeof(pte_t); i++) {
		pte = old[i];
		if (!(pte & PTE_V))
			continue;
		va = base | ((uint64)i << (PGSHIFT + 9 * level));
		if (va == USER_SIGRETURN)
			continue;
		if (!(pte & (PTE_R | PTE_W | PTE_X))) {
			if (level == 0 ||
			    vm_copy_walk((pagedir_t)PTE2PA(pte), new,
			                 level - 1, va, vmas,
			                 parent_changed) < 0)
				return -1;
			continue;
		}
		if (!(pte & (PTE_U | PTE_SW_USER)))
			continue;
		area = vma_find(vmas, va);
		if (!area)
			return -1;
		pa = PTE2PA(pte);
		if (palloc_get((void *)pa) < 0)
			return -1;
		child_pte = pte;
		if ((area->flags & 0xf) == LINUX_MAP_PRIVATE &&
		    (pte & PTE_W)) {
			child_pte = (pte & ~PTE_W) | PTE_SW_COW;
			old[i] = child_pte;
			*parent_changed = 1;
		}
		if (vm_map(new, va, pa, PGSIZE, child_pte & 0x3ff) < 0) {
			pfree((void *)pa);
			return -1;
		}
	}
	return 0;
}

int vm_copy(pagedir_t old, pagedir_t new, const struct vma_set *vmas)
{
	int parent_changed = 0;

	if (vm_copy_walk(old, new, 2, 0, vmas, &parent_changed) < 0) {
		vm_free_user(new);
		if (parent_changed)
			cpu_tlb_flush_all();
		return -1;
	}
	if (parent_changed)
		cpu_tlb_flush_all();
	return 0;
}

static void vm_free_user_walk(pagedir_t pgdir, int level)
{
	pte_t pte;
	int i;

	for (i = 0; i < PGSIZE / sizeof(pte_t); i++) {
		pte = pgdir[i];
		if (!(pte & PTE_V))
			continue;
		if (!(pte & (PTE_R | PTE_W | PTE_X))) {
			if (level > 0)
				vm_free_user_walk((pagedir_t)PTE2PA(pte),
				                  level - 1);
			continue;
		}
		if (pte & (PTE_U | PTE_SW_USER)) {
			pfree((void *)PTE2PA(pte));
			pgdir[i] = 0;
		}
	}
}

void vm_free_user(pagedir_t pgdir)
{
	vm_free_user_walk(pgdir, 2);
}

static int copyout_internal(pagedir_t pgdir, uint64 dstva, char *src,
			    uint64 len, int fault)
{
	void *pinned_page;
        uint64 n, va0, pa0;

        while(len > 0){
                va0 = PGROUNDDOWN(dstva);
		pa0 = user_copy_va2pa_pinned(pgdir, va0, PTE_W, fault,
					     &pinned_page);
                if(pa0 == 0)
                        return -1;
                n = PGSIZE - (dstva - va0);
                if(n > len)
                        n = len;
                memmove((void *)(pa0 + (dstva - va0)), src, n);
		pfree(pinned_page);

                len -= n;
                src += n;
                dstva = va0 + PGSIZE;
        }
        return 0;
}

int copyout(pagedir_t pgdir, uint64 dstva, char *src, uint64 len)
{
	return copyout_internal(pgdir, dstva, src, len, 1);
}

int copyout_nofault(pagedir_t pgdir, uint64 dstva, char *src, uint64 len)
{
	return copyout_internal(pgdir, dstva, src, len, 0);
}

int copyin(pagedir_t pgdir, char* dst, uint64 srcva, uint64 len)
{
	void *pinned_page;
         uint64 n, va0, pa0;

        while(len > 0){
                va0 = PGROUNDDOWN(srcva);
		pa0 = user_copy_va2pa_pinned(pgdir, va0, PTE_R, 1,
					     &pinned_page);
                if(pa0 == 0)
                        return -1;
                n = PGSIZE - (srcva - va0);
                if(n > len)
                        n = len;
                memmove(dst, (void *)(pa0 + (srcva - va0)),n);
		pfree(pinned_page);

                len -= n;
                dst += n;
                srcva = va0 + PGSIZE;
        }
        return 0;
}

int copyinstr(pagedir_t pgdir, char *dst, uint64 srcva, uint64 max)
{
	void *pinned_page;
        uint64 n, va0, pa0;
        int got_null = 0;

        while(got_null == 0 && max > 0) {
                va0 = PGROUNDDOWN(srcva);
		pa0 = user_copy_va2pa_pinned(pgdir, va0, PTE_R, 1,
					     &pinned_page);
                if(pa0 == 0)
                        return -1;
                n = PGSIZE - (srcva - va0);
                if(n > max)
                        n = max;

                char *p = (char *) (pa0 + (srcva - va0));
                while(n > 0) {
                        if(*p == '\0') {
                                *dst = '\0';
                                got_null = 1;
                                break;
                        } else {
                                *dst = *p;
                        }
                        --n;
                        --max;
                        p++;
                        dst++;
                }
		pfree(pinned_page);

                srcva = va0 + PGSIZE;
        }
        if(got_null) {
                return 0;
        } else {
                return -1;
        }
}


void kvm_create(void)
{
        kernel_pgdir = kernel_pagedir_t_create();
}

void kvm_init(void)
{
        
        /* Wait page-table operation */
        sfence_vma();

        /* Load it */
        satp_w(MAKE_SATP(kernel_pgdir));

        /* Refresh the 'satp' register */
        sfence_vma(); 
}

int kvm_mapping_selftest(void)
{
	uint64 candidate, end, start;
	int i, level;
	pte_t *pte;

	for (i = 0; i < palloc_managed_range_count(); i++) {
		if (palloc_managed_range_get(i, &start, &end) < 0 ||
		    kvm_va2pa(start) != start ||
		    kvm_va2pa(end - PGSIZE) != end - PGSIZE)
			return -1;
		candidate = (start + sv39_level_size(2) - 1) &
			    ~(sv39_level_size(2) - 1);
		if (candidate >= start && candidate <= end &&
		    sv39_level_size(2) <= end - candidate) {
			pte = vm_walk(kernel_pgdir, candidate, 0, 0, &level);
			if (!pte || level != 2)
				return -1;
		}
	}
	return 0;
}

int kvm_map_mmio(uint64 address, uint64 size)
{
	uint64 end, page;

	if (!size || address >= MAXVA || size > MAXVA - address)
		return -1;
	page = PGROUNDDOWN(address);
	end = PGROUNDUP(address + size);
	for (; page < end; page += PGSIZE) {
		if (vm_mapped(kernel_pgdir, page))
			continue;
		if (vm_map(kernel_pgdir, page, page, PGSIZE,
		           PTE_R | PTE_W) < 0)
			return -1;
	}
	sfence_vma();
	return 0;
}
