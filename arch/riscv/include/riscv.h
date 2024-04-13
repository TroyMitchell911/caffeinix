#ifndef __CAFFEINIX_ARCH_RISCV_RISCV_H
#define __CAFFEINIX_ARCH_RISCV_RISCV_H

#include <typedefs.h>

/* Previous mode of machine mode */
#define MSTATUS_MPP_MASK        (3L << 11) 
/* Machine mode */
#define MSTATUS_MPP_M           (3L << 11)
/* Supervisor mode */
#define MSTATUS_MPP_S           (1L << 11)
/* User mode */
#define MSTATUS_MPP_U           (0L << 11)

#define PGSHIFT                 12  // bits of offset within a page
#define PGSIZE                  4096

#define PGROUNDUP(sz)   (((sz)+PGSIZE-1) & ~(PGSIZE-1))
#define PGROUNDDOWN(a)  (((a)) & ~(PGSIZE-1))

/* Extract the three 9-bit page table indices from a virtual address. */
#define PTEXMASK                0x1FF // 9 bits
#define PTEXSHIFT(level)        (PGSHIFT+(9*(level)))
#define PTEX(level, va)         ((((uint64) (va)) >> PTEXSHIFT(level)) & PTEXMASK)

/* shift a physical address to the right place for a PTE */
#define PA2PTE(pa)             ((((uint64)pa) >> 12) << 10)
#define PTE2PA(pte)            (((pte) >> 10) << 12)

#define PTE_V                   (1L << 0)         // valid
#define PTE_R                   (1L << 1)
#define PTE_W                   (1L << 2)
#define PTE_X                   (1L << 3)
#define PTE_U                   (1L << 4) // user can access

#define SATP_SV39               (8L << 60)
#define MAKE_SATP(pgdir)        (SATP_SV39 | (((uint64)pgdir) >> 12))

/*      
        one beyond the highest possible virtual address.
        MAXVA is actually one bit less than the max allowed by
        Sv39, to avoid having to sign-extend virtual addresses
        that have the high bit set.
*/
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))


typedef uint64 pte_t;
typedef uint64 *pagedir_t;

/* Read hart id */
static inline uint64 mhartid_r(void)
{
        uint64 id;
        asm volatile("csrr %0, mhartid" : "=r"(id) :);
        return id;
}

/* Write status */
static inline void mstatus_w(uint64 s)
{
        asm volatile("csrw mstatus, %0" : : "r"(s));
}

/* Read status */
static inline uint64 mstatus_r(void)
{
        uint64 s;
        asm volatile("csrr %0, mstatus" : "=r"(s) :);
        return s;
}

static inline void mepc_w(uint64 epc)
{
        asm volatile("csrw mepc, %0" : : "r"(epc));
}

static inline void satp_w(uint64 v)
{
        asm volatile("csrw satp, %0" : : "r"(v));
}

static inline void pmpaddr0_w(uint64 v)
{
        asm volatile("csrw pmpaddr0, %0" : : "r"(v));
}

static inline void pmpcfg0_W(uint64 v)
{
        asm volatile("csrw pmpcfg0, %0" : : "r"(v));
}

static inline void sfence_vma(void)
{
        asm volatile("sfence.vma zero, zero");
}

#endif