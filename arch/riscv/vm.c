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
#include <mystring.h>
#include <debug.h>
#include <process.h>
#include <printf.h>

/* Defination in kernel.ld */
extern char etext[];

extern char trampoline[];

static pagedir_t kernel_pgdir;

pte_t *PTE(pagedir_t pgdir, uint64 va, int flag)
{
        int i;
        pte_t *pte;
        /* The value of va can't be more than MAXVA */
        if(va >= MAXVA)
                PANIC("PTE");
        /* 
                Starting from a high address,
                retrieve the page table pointed to by the page table entry
        */
        for(i = 2; i > 0; i--) {
                /* Get the page table pointed to by the page table entry */
                pte = &pgdir[PTEX(i, va)];
                /* If the page-table exists */
                if((*pte) & PTE_V) {
                        /* Store the physical address of page-table into pgdir */
                        pgdir = (pagedir_t)PTE2PA(*pte);
                } else {
                        if(flag == 0 || (pgdir = (pte_t*)palloc()) == 0){
                                return 0;
                        }
                        /* The variable 'pgdir' has been alloced above */
                        memset(pgdir, 0, PGSIZE);
                        /* Store the pte */
                        *pte = PA2PTE(pgdir) | PTE_V;
                }
        }
        /* Return the lowest level of pte */
        return (pte_t*)&pgdir[PTEX(0, va)];
}

uint64 va2pa(pagedir_t pgdir, uint64 va)
{
        pte_t *pte;
        uint64 pa;

        if(va >= MAXVA)
                return 0;

        pte = PTE(pgdir, va, 0);
        if(pte == 0)
                return 0;
        if((*pte & PTE_V) == 0)
                return 0;
        if((*pte & PTE_U) == 0)
                return 0;
        pa = PTE2PA(*pte);
                return pa;
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
        /* Alloc the physical memory for page-table */
        pagedir_t pgdir = (pagedir_t)palloc();
        
        memset(pgdir, 0, PGSIZE);

	/* Map the QEMU virt machine's virtio MMIO transports. */
	vm_map(pgdir, VIRTIO0, VIRTIO0, VIRTIO_MMIO_SLOTS * PGSIZE,
	       PTE_R | PTE_W);
        /* Map the UART0 */
        vm_map(pgdir, UART0, UART0, PGSIZE, PTE_R | PTE_W);
        /* Map the trampoline */        
        vm_map(pgdir, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_X | PTE_R);
        /* PLIC */
        vm_map(pgdir, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
        /* Map the text */
        vm_map(pgdir, KERNEL_BASE, KERNEL_BASE, (uint64)etext - KERNEL_BASE, PTE_R | PTE_X);
        /* Map the data and the rest of physical DRAM */
        vm_map(pgdir, (uint64)etext, (uint64)etext, PHY_MEM_STOP - (uint64)etext, PTE_R | PTE_W);

        map_kernel_stack(pgdir);
        return pgdir;
}

pagedir_t pagedir_alloc(void)
{
        /* Malloc memory for page-talble */
        pagedir_t pgdir = (pagedir_t)palloc();
        if(!pgdir) {
                return 0;
        }
        /* Clear the memory */
        memset(pgdir, 0, PGSIZE);

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
                                To ensure this, we should cancel V in the unmap function.
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
	sfence_vma();
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
        uint64 addr;
        void* mem;
        if(newsz <= oldsz)
                return oldsz;

        oldsz = PGROUNDUP(oldsz);
	for(addr = oldsz; addr < newsz;addr += PGSIZE) {
                mem = palloc();
                if(!mem) {
                        vm_dealloc(pgdir, addr, oldsz);
                        return 0;
                }
                memset(mem, 0, PGSIZE);
                if(vm_map(pgdir, (uint64)addr, (uint64)mem, PGSIZE, PTE_R | PTE_U | eperm) != 0) {
                        printf("???\n");
                        pfree(mem);
                        vm_dealloc(pgdir, addr, oldsz);
                        return 0;
		}
	}

	sfence_vma();
        return newsz;
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
			uint64 base)
{
	uint64 mem, pa, va;
	pte_t pte;
	int i;

	for (i = 0; i < PGSIZE / sizeof(pte_t); i++) {
		pte = old[i];
		if (!(pte & PTE_V))
			continue;
		va = base | ((uint64)i << (PGSHIFT + 9 * level));
		if (!(pte & (PTE_R | PTE_W | PTE_X))) {
			if (level == 0 ||
			    vm_copy_walk((pagedir_t)PTE2PA(pte), new,
			                 level - 1, va) < 0)
				return -1;
			continue;
		}
		if (!(pte & PTE_U))
			continue;
		pa = PTE2PA(pte);
		mem = (uint64)palloc();
		if (!mem)
			return -1;
		memmove((char *)mem, (char *)pa, PGSIZE);
		if (vm_map(new, va, mem, PGSIZE, pte & 0x3ff) < 0) {
			pfree((void *)mem);
			return -1;
		}
	}
	return 0;
}

int vm_copy(pagedir_t old, pagedir_t new)
{
	if (vm_copy_walk(old, new, 2, 0) < 0) {
		vm_free_user(new);
		return -1;
	}
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
		if (pte & PTE_U) {
			pfree((void *)PTE2PA(pte));
			pgdir[i] = 0;
		}
	}
}

void vm_free_user(pagedir_t pgdir)
{
	vm_free_user_walk(pgdir, 2);
}

int copyout(pagedir_t pgdir, uint64 dstva, char* src, uint64 len)
{
        uint64 n, va0, pa0;

        while(len > 0){
                va0 = PGROUNDDOWN(dstva);
                pa0 = va2pa(pgdir, va0);
                if(pa0 == 0)
                        return -1;
                n = PGSIZE - (dstva - va0);
                if(n > len)
                        n = len;
                memmove((void *)(pa0 + (dstva - va0)), src, n);

                len -= n;
                src += n;
                dstva = va0 + PGSIZE;
        }
        return 0;
}

int copyin(pagedir_t pgdir, char* dst, uint64 srcva, uint64 len)
{
         uint64 n, va0, pa0;

        while(len > 0){
                va0 = PGROUNDDOWN(srcva);
                pa0 = va2pa(pgdir, va0);
                if(pa0 == 0)
                        return -1;
                n = PGSIZE - (srcva - va0);
                if(n > len)
                        n = len;
                memmove(dst, (void *)(pa0 + (srcva - va0)),n);

                len -= n;
                dst += n;
                srcva = va0 + PGSIZE;
        }
        return 0;
}

int copyinstr(pagedir_t pgdir, char *dst, uint64 srcva, uint64 max)
{
        uint64 n, va0, pa0;
        int got_null = 0;

        while(got_null == 0 && max > 0) {
                va0 = PGROUNDDOWN(srcva);
                pa0 = va2pa(pgdir, va0);
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
