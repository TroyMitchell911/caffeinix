#include <palloc.h>
#include <mem_layout.h>
#include <vm.h>
#include <string.h>
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

        /* Map virtio mmio disk interface */
        vm_map(pgdir, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
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

        process_map_kernel_stack(pgdir);
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
                if(((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X))) == 0) {
                        sub_pgdir_pa = PTE2PA(pte);
                        pagedir_free((pagedir_t)sub_pgdir_pa);
                        pgdir[i] = 0;
                } else if((pte & PTE_V)) {
                        /* 
                                We can't accept a PTE has the flag 'V'.
                                To ensure this, we should cancel V in the unmap function.
                        */
                        printf("%d\n", pte);
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

        for(addr = va; addr <= va + npages * PGSIZE; addr += PGSIZE) {
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