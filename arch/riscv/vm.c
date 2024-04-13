#include <riscv.h>
#include <palloc.h>
#include <mem_layout.h>
#include <vm.h>
#include <string.h>
#include <debug.h>

/* Defination in kernel.ld */
extern char etext[];

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

void vmmap(pagedir_t pgdir, uint64 va, uint64 pa, uint64 size, int perm)
{
        uint64 start, end;
        pte_t *pte;
        /* Size can't be 0 */
        if(!size) {
                PANIC("vmmap size");
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
                        PANIC("vmmap");
                /* The pte can't include the bit PTE_V */
                if(*pte & PTE_V) {
                        PANIC("vmmap pte");
                }
                /* Set the PTE */
                *pte = PA2PTE(pa) | PTE_V | perm;
                if(start == end)
                        break;
                start += PGSIZE;
                pa += PGSIZE;
        }
}

static pagedir_t kernel_pagedir_t_create(void)
{
        /* Alloc the physical memory for page-table */
        pagedir_t pgdir = (pagedir_t)palloc();
        
        memset(pgdir, 0, PGSIZE);

        /* map the text */
        vmmap(pgdir, KERNEL_BASE, KERNEL_BASE, (uint64)etext - KERNEL_BASE, PTE_R | PTE_X);
        /* map the data and the rest of physical DRAM */
        vmmap(pgdir, (uint64)etext, (uint64)etext, PHY_MEM_STOP - (uint64)etext, PTE_R | PTE_W);

        return pgdir;
}

void vm_init(void)
{
        pagedir_t kernel_pgdir;
        /* Wait page-table operation */
        sfence_vma();

        /* Create page-table then load it */
        kernel_pgdir = kernel_pagedir_t_create();
        satp_w(MAKE_SATP(kernel_pgdir));

        /* Refresh the 'satp' register */
        sfence_vma(); 
}