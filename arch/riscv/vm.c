/*
 * RISC-V SV39 page-table management and fault-aware user-memory copying.
 *
 * User mappings are changed under the owning process mmap_lock.  Kernel page
 * tables are built before secondary harts consume them; walker allocations
 * are physical pages and leaf PTEs may use SV39 huge-page levels.
 *
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

/* Linker-defined end of executable kernel text. */
extern char etext[];

extern char trampoline[];

static pagedir_t kernel_pgdir;

/* Walk to a requested SV39 level, allocating intermediate tables if allowed. */
static pte_t *vm_walk(pagedir_t pgdir, uint64 va, int allocate,
		      int target_level, int *leaf_level)
{
	int level;
	pte_t *pte;
	pagedir_t next;

	if (va >= MAXVA || target_level < 0 ||
	    target_level > SV39_LEVEL_MAX)
		return 0;
	/* Descend from the root and optionally allocate intermediate tables. */
	for (level = SV39_LEVEL_MAX; level > target_level; level--) {
		pte = &pgdir[PTEX(level, va)];
		if (*pte & PTE_V) {
			if (*pte & (PTE_R | PTE_W | PTE_X)) {
				if (leaf_level)
					*leaf_level = level;
				return pte;
			}
			pgdir = (pagedir_t)PTE2PA(*pte);
                } else {
			if (!allocate || (next = palloc_zero()) == 0)
                                return 0;
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

/* Translate an accessible user page and set accessed/dirty state for writes. */
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
	if (permissions & PTE_W)
		__atomic_fetch_or(pte, PTE_A | PTE_D, __ATOMIC_RELAXED);
	leaf_size = sv39_level_size(level);
	return PTE2PA(*pte) +
	       (va & (leaf_size - 1) & ~(PGSIZE - 1));
}

/* Fault, validate, and pin a user page for one copy operation. */
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

/**
 * va2pa() - Find the physical page containing an accessible user address
 * @pgdir: Stable user page directory.
 * @va: User virtual address below MAXVA.
 *
 * The within-page byte offset is not included. The result is not a retained
 * page reference and does not establish read or write permission.
 *
 * Context: Caller prevents mapping removal; does not allocate or sleep.
 * Return: Physical 4 KiB page base, or zero if not present and
 *         user-accessible.
 */
uint64 va2pa(pagedir_t pgdir, uint64 va)
{
	return user_va2pa(pgdir, va, 0);
}

/**
 * kvm_va2pa() - Translate a kernel virtual address including its byte offset
 * @va: Kernel virtual address below MAXVA.
 *
 * Handles huge leaves without allocating or taking a page reference.
 *
 * Context: After kernel page-table construction; mappings must remain stable.
 * Return: Physical byte address, or zero if there is no valid leaf.
 */
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

/* Install one aligned SV39 leaf, rejecting any pre-existing mapping. */
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

/* Cover a range using the largest legal SV39 leaves. */
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
        if(!size) {
                PANIC("vm_map size");
        }
        start = PGROUNDDOWN(va);
        end = PGROUNDDOWN(va + size - 1);
        
        for(;;) {
                /* 
                        Obtain the address of the page table entry 
                        with the virtual address in the page table  
                */
                if((pte = PTE(pgdir, start, 1)) == 0)
                        return -1;
                if(*pte & PTE_V) {
                        PANIC("vm_map remap");
                }
                *pte = PA2PTE(pa) | PTE_V | perm;
                if(start == end)
                        break;
                start += PGSIZE;
                pa += PGSIZE;
        }
        return 0;
}

/* Build the kernel mapping before it is published in satp. */
static pagedir_t kernel_pagedir_t_create(void)
{
	uint64 finish, start;
	int i;

	pagedir_t pgdir = (pagedir_t)palloc_zero();

	if (!pgdir)
		PANIC("allocate kernel page table");

        vm_map(pgdir, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_X | PTE_R);
        /* PLIC */
        vm_map(pgdir, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
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
	pagedir_t pgdir = (pagedir_t)palloc_zero();
        if(!pgdir) {
                return 0;
        }

        return pgdir;
}
/**
 * pagedir_free() - Release an empty hierarchy of page-table pages
 * @pgdir: Non-NULL allocated root whose leaf mappings were removed.
 *
 * Recursively frees intermediate tables and the root. A valid remaining leaf
 * is a fatal caller error; this is not a substitute for releasing mapped
 * pages.
 *
 * Context: Exclusive teardown of a page directory no CPU can still use.
 */
void pagedir_free(pagedir_t pgdir)
{
        int i;
        pte_t pte;
        uint64 sub_pgdir_pa;
        for(i = 0; i < PGSIZE / 8; i ++) {
                pte = pgdir[i];
                if((pte & PTE_V) && ((pte & (PTE_R|PTE_W|PTE_X)) == 0)) {
                        sub_pgdir_pa = PTE2PA(pte);
                        pagedir_free((pagedir_t)sub_pgdir_pa);
                        pgdir[i] = 0;
                } else if((pte & PTE_V)) {
                        /* Leaves must be unmapped before freeing tables. */
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
                /* A non-leaf entry owns a table, not a mapped data page. */
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

/**
 * vm_unmap_range() - Drop mapped page references across a byte range
 * @pgdir: User directory containing only 4 KiB leaves in the range.
 * @va: Virtual start, rounded down to its containing page.
 * @size: Byte length; zero does nothing. The rounded end must not wrap.
 *
 * Skips holes, clears present leaves, drops each physical-page reference, and
 * flushes online CPUs' TLBs. Intermediate tables remain allocated.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 */
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

/**
 * vm_dealloc() - Shrink the resident part of a contiguous user allocation
 * @pgdir: Directory owning 4 KiB user mappings.
 * @oldsz: Previous allocation end in bytes.
 * @newsz: Requested smaller byte end.
 *
 * Keeps the partial final page containing @newsz, and removes fully trailing
 * pages. Does not update VMA metadata.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 * Return: The new byte end, or @oldsz if @newsz would not shrink it.
 */
uint64 vm_dealloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz)
{
        if(newsz >= oldsz)
                return oldsz;

        if(PGROUNDUP(newsz) < PGROUNDUP(oldsz))
		vm_unmap_range(pgdir, PGROUNDUP(newsz),
		               PGROUNDUP(oldsz) - PGROUNDUP(newsz));
        return newsz;
}


/**
 * vm_alloc() - Grow a zero-filled contiguous user allocation
 * @pgdir: Directory exclusively serialized by the caller.
 * @oldsz: Old allocation end in bytes.
 * @newsz: Requested allocation end, with rounding bounded below MAXVA.
 * @eperm: Additional PTE permission bits; read/user bits are added.
 *
 * Allocates missing pages eagerly; callers update VMA metadata separately.
 * Failure unwinds leaves added by this allocation, not intermediate tables.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 * Return: Requested byte end, old end for no growth, or zero on failure.
 */
uint64 vm_alloc(pagedir_t pgdir, uint64 oldsz, uint64 newsz, int eperm)
{
        if(newsz <= oldsz)
                return oldsz;

        if (vm_alloc_range(pgdir, PGROUNDUP(oldsz), PGROUNDUP(newsz),
			   eperm) < 0)
		return 0;
        return newsz;
}

/**
 * vm_alloc_range() - Allocate readable user pages with extra permissions
 * @pgdir: Directory owning the new mappings.
 * @start: Page-aligned inclusive virtual start.
 * @end: Page-aligned exclusive virtual end, at most MAXVA.
 * @eperm: Additional PTE permission bits, such as PTE_W or PTE_X.
 *
 * Delegates to vm_alloc_user_range() with PTE_R and PTE_U added.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 * Return: Zero on success, -1 for invalid range, conflict, or allocation
 *         failure.
 */
int vm_alloc_range(pagedir_t pgdir, uint64 start, uint64 end, int eperm)
{
	return vm_alloc_user_range(pgdir, start, end,
				   PTE_R | PTE_U | eperm);
}

/**
 * vm_alloc_user_range() - Allocate and zero all pages of an unmapped range
 * @pgdir: Directory owning the resulting page references.
 * @start: Page-aligned inclusive start.
 * @end: Page-aligned exclusive end no larger than MAXVA.
 * @permissions: PTE access bits; at least R/W/X, and W requires R.
 *
 * This is eager allocation, not a VMA reservation. Adds PTE_SW_USER for
 * ownership tracking. Allocation failure drops newly installed leaves but may
 * retain intermediate tables; callers must not overlap valid hidden mappings.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 * Return: Zero on success, -1 for invalid input, existing mapping, or
 *         exhaustion.
 */
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

/**
 * vm_alloc_load_range() - Merge a loader boundary page then allocate the rest
 * @pgdir: Unpublished loader page directory.
 * @start: Page-aligned segment start; only the first page may be shared.
 * @end: Page-aligned exclusive end no larger than MAXVA.
 * @permissions: Valid leaf PTE access bits for the load segment.
 *
 * An already mapped first page must carry PTE_SW_USER. Permission changes to
 * that page are not rolled back if allocation of later pages fails; the
 * loader discards the failed image.
 *
 * Context: Exclusive image construction, before user execution.
 * Return: Zero on success or -1 on invalid layout, conflict, or exhaustion.
 */
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

/**
 * vm_user_pa() - Translate an owned user leaf including its byte offset
 * @pgdir: Stable directory with 4 KiB user leaves.
 * @va: Virtual address below MAXVA.
 *
 * Does not require PTE_U or check requested access permissions; useful for
 * loader-owned pages hidden from userspace. Takes no page reference.
 *
 * Context: Caller keeps the mapping alive; no allocation or sleeping.
 * Return: Physical byte address, or zero without a valid PTE_SW_USER leaf.
 */
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

/**
 * vm_protect_user_range() - Apply VMA-derived permissions to resident pages
 * @pgdir: Process directory whose VMAs already describe the new policy.
 * @start: Page-aligned inclusive virtual start.
 * @end: Page-aligned exclusive end; must exceed @start.
 * @permissions: Valid PTE access bits; hidden pages omit PTE_U.
 * @vmas: Stable VMA set covering every present user page in the range.
 *
 * Skips nonresident pages, preserves COW for shared private pages, and defers
 * shared-file writable upgrades to the fault path. Missing VMA metadata can
 * fail after earlier PTE changes; callers must supply complete metadata.
 * Successful changes flush online CPUs' TLBs.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 * Return: Zero on success or -1 on invalid input or inconsistent mappings.
 */
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
			} else if (area->origin == VMA_FILE_BACKED &&
				   (area->flags & 0xf) == LINUX_MAP_SHARED &&
				   !(*pte & PTE_W)) {
				page_permissions &= ~PTE_W;
				software = 0;
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

/**
 * vm_clear() - Revoke direct user access to an existing PTE
 * @pgdir: Directory whose page-table path exists for @va.
 * @va: Virtual address selecting that PTE.
 *
 * Clears only PTE_U. Does not drop a page reference or flush a TLB; the
 * caller handles publication. A missing page-table path panics.
 *
 * Context: Caller holds the owning mmap_lock or exclusively owns an
 *          unpublished page directory.
 */
void vm_clear(pagedir_t pgdir, uint64 va)
{
        pte_t* pte = PTE(pgdir, va, 0);
        if(!pte) {
                PANIC("vm_clear");
        } else {
                *pte &= ~PTE_U;
        }
}

/* Recursively duplicate page-table structure and establish COW leaf entries. */
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

/* Recursively count present user leaf pages, including huge leaves. */
static uint64 vm_user_resident_walk(pagedir_t pgdir, int level)
{
	uint64 pages = 0;
	pte_t pte;
	int index;

	for (index = 0; index < PGSIZE / sizeof(pte_t); index++) {
		pte = pgdir[index];
		if (!(pte & PTE_V))
			continue;
		if (!(pte & (PTE_R | PTE_W | PTE_X))) {
			if (level > 0)
				pages += vm_user_resident_walk(
					(pagedir_t)PTE2PA(pte), level - 1);
			continue;
		}
		if (pte & PTE_U)
			pages += sv39_level_size(level) / PGSIZE;
	}
	return pages;
}

/**
 * vm_user_resident_pages() - Count present user-accessible leaf pages
 * @pgdir: Stable process directory, or NULL for an empty count.
 *
 * Counts only PTE_U leaves, not hidden PTE_SW_USER-only pages or page-table
 * storage. Does not acquire references.
 *
 * Context: Caller prevents concurrent mapping changes; does not sleep.
 * Return: Resident page count in 4 KiB units, including huge-leaf spans.
 */
uint64 vm_user_resident_pages(pagedir_t pgdir)
{
	return pgdir ? vm_user_resident_walk(pgdir, SV39_LEVEL_MAX) : 0;
}

/* Drop user leaves; leave the table hierarchy for pagedir_free(). */
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

/**
 * vm_free_user() - Release owned user leaf pages during teardown
 * @pgdir: Non-NULL directory containing user 4 KiB mappings.
 *
 * Clears PTE_U/PTE_SW_USER leaves and drops their references. Keeps
 * intermediate tables and kernel-only leaves; does not perform TLB
 * invalidation. pagedir_free() is a separate final step after remaining
 * leaves are unmapped.
 *
 * Context: Exclusive teardown or serialized removal with no executing users.
 */
void vm_free_user(pagedir_t pgdir)
{
	vm_free_user_walk(pgdir, 2);
}

/* Copy to user memory, optionally faulting pages before each chunk. */
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

/**
 * copyout_nofault() - Copy only through present writable user mappings
 * @pgdir: Directory retained by the caller during the copy.
 * @dstva: User destination byte address.
 * @src: Kernel source readable for @len bytes.
 * @len: Byte count; zero leaves pointers untouched.
 *
 * Pins and revalidates each physical page but does not fault missing or COW
 * pages. Holding the mapping stable for an atomic operation remains the
 * caller's responsibility.
 *
 * Context: Non-sleeping copy path; takes page-reference spinlocks.
 * Return: Zero on a full copy, -1 on an inaccessible page; a prefix may be
 *         written.
 */
int copyout_nofault(pagedir_t pgdir, uint64 dstva, char *src, uint64 len)
{
	return copyout_internal(pgdir, dstva, src, len, 0);
}

/**
 * vm_prefault_user_write() - Resolve writable pages before a later user copy
 * @pgdir: Current process directory for fault resolution.
 * @address: User byte-range start below MAXVA.
 * @length: Byte length; zero succeeds, wraparound is rejected.
 *
 * References are dropped after each check. This does not pin the complete
 * range or guarantee a later copy cannot fail if a sibling changes mappings.
 *
 * Context: Sleepable thread context; do not hold mmap_lock across fault
 *          resolution.
 * Return: Zero if each page was writable when checked, otherwise -1.
 */
int vm_prefault_user_write(pagedir_t pgdir, uint64 address, uint64 length)
{
	void *pinned_page;
	uint64 end, page;

	if (!length)
		return 0;
	if (address >= MAXVA || length > MAXVA - address)
		return -1;
	end = address + length;
	for (page = PGROUNDDOWN(address); page < end; page += PGSIZE) {
		if (!user_copy_va2pa_pinned(pgdir, page, PTE_W, 1,
					    &pinned_page))
			return -1;
		pfree(pinned_page);
	}
	return 0;
}

/**
 * copyin() - Copy readable user bytes into kernel storage
 * @pgdir: Retained user directory; faults are resolved only for cur_proc().
 * @dst: Kernel destination writable for @len bytes.
 * @srcva: User source byte address.
 * @len: Requested byte count; zero is a no-op.
 *
 * Pins and validates one page at a time. A different process's directory must
 * already contain the required readable mappings.
 *
 * Context: Sleepable thread context for demand faults; do not hold mmap_lock.
 * Return: Zero on a full copy or -1 after any successfully copied prefix.
 */
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

/**
 * copyinstr() - Copy a terminated user string within a byte bound
 * @pgdir: Retained user directory; only the current process may fault.
 * @dst: Kernel destination with room for @max bytes.
 * @srcva: User string start address.
 * @max: Maximum copied bytes including the NUL terminator.
 *
 * On failure a copied prefix need not be NUL-terminated. Each page is pinned
 * only while its bytes are inspected.
 *
 * Context: Sleepable thread context for demand faults; do not hold mmap_lock.
 * Return: Zero including NUL, -1 on inaccessible input or no NUL within the
 *         bound.
 */
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
        
        /* Publish preceding page-table stores before enabling this root. */
        sfence_vma();

        satp_w(MAKE_SATP(kernel_pgdir));

        /* Discard translations cached under the previous address space. */
        sfence_vma(); 
}

/**
 * kvm_mapping_selftest() - Check direct-map endpoints and huge-leaf selection
 *
 * Context: Early boot after kernel mapping construction; does not allocate.
 * Return: Zero when managed-region endpoints and eligible 1 GiB leaves match,
 *         else -1.
 */
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

/**
 * kvm_map_mmio() - Add identity-mapped writable kernel MMIO pages
 * @address: Physical byte address below MAXVA.
 * @size: Nonzero byte extent; address plus size must not overflow MAXVA.
 *
 * Rounds to page boundaries and skips already mapped pages without changing
 * their attributes. Flushes only the local TLB; caller must arrange safe
 * publication to other CPUs.
 *
 * Context: Serialized driver/early boot setup after kvm_create(); may
 *          allocate tables.
 * Return: Zero or -1 on invalid extent or mapping failure; earlier pages
 *         remain mapped.
 */
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
