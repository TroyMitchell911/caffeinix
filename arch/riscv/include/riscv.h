#ifndef __CAFFEINIX_ARCH_RISCV_RISCV_H
#define __CAFFEINIX_ARCH_RISCV_RISCV_H

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

#ifndef __ASSEMBLER__
#include <typedefs.h>

typedef uint64 pte_t;
typedef uint64 *pagedir_t;

/* Read hart id */
static inline uint64 mhartid_r(void)
{
        uint64 id;
        asm volatile("csrr %0, mhartid" : "=r"(id) :);
        return id;
}
/* machine-mode interrupt enable. */
#define MSTATUS_MIE (1L << 3)     
/* Previous mode of machine mode */
#define MSTATUS_MPP_MASK        (3L << 11) 
/* Machine mode */
#define MSTATUS_MPP_M           (3L << 11)
/* Supervisor mode */
#define MSTATUS_MPP_S           (1L << 11)
/* User mode */
#define MSTATUS_MPP_U           (0L << 11)

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

static inline uint64 satp_r(void)
{
        uint64 s;
        asm volatile("csrr %0, satp" : "=r"(s) :);
        return s;
}

static inline void pmpaddr0_w(uint64 v)
{
        asm volatile("csrw pmpaddr0, %0" : : "r"(v));
}

static inline void pmpcfg0_w(uint64 v)
{
        asm volatile("csrw pmpcfg0, %0" : : "r"(v));
}

static inline void sfence_vma(void)
{
        asm volatile("sfence.vma zero, zero");
}

static inline void mtvec_w(uint64 v)
{
        asm volatile("csrw mtvec, %0" : : "r"(v));
}

static inline void mscratch_w(uint64 v)
{
        asm volatile("csrw mscratch, %0" : : "r"(v));
}

/* Supervisor Interrupt Enable */
#define SIE_SEIE (1L << 9)  /* external */
#define SIE_STIE (1L << 5)  /* timer */
#define SIE_SSIE (1L << 1)  /* software */
static inline uint64 sie_r(void)
{
        uint64 r;
        asm volatile("csrr %0, sie" : "=r"(r) :);
        return r;
}

static inline void sie_w(uint64 v)
{
        asm volatile("csrw sie, %0" : : "r"(v));
}

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4) // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)  // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)  // User Interrupt Enable

static inline uint64 sstatus_r()
{
        uint64 v;
        asm volatile("csrr %0, sstatus" : "=r" (v) );
        return v;
}

static inline void sstatus_w(uint64 v)
{
  asm volatile("csrw sstatus, %0" : : "r" (v));
}

static inline uint64 scause_r(void)
{
        uint64 r;
        asm volatile("csrr %0, scause" : "=r"(r) :);
        return r;
}

static inline uint64 stval_r(void)
{
        uint64 r;
        asm volatile("csrr %0, stval" : "=r"(r) :);
        return r;
}

static inline uint64 sepc_r(void)
{
        uint64 r;
        asm volatile("csrr %0, sepc" : "=r"(r) :);
        return r;
}

static inline void sepc_w(uint64 v)
{
  asm volatile("csrw sepc, %0" : : "r" (v));
}


static inline void sip_w(uint64 v)
{
  asm volatile("csrw sip, %0" : : "r" (v));
}

static inline uint64 sip_r(void)
{
        uint64 r;
        asm volatile("csrr %0, sip" : "=r"(r) :);
        return r;
}

static inline void tp_w(uint64 v)
{
        asm volatile("mv tp, %0" : : "r"(v));
}

static inline uint64 tp_r(void)
{
        uint64 r;
        asm volatile("mv %0, tp" : "=r"(r) :);
        return r;
}

static inline uint64 intr_status(void)
{
        uint64 x = sstatus_r();
        return (x & SSTATUS_SIE) != 0;
}

static inline void intr_on(void)
{
        sstatus_w(sstatus_r() | SSTATUS_SIE);
}

static inline void intr_off(void)
{
        sstatus_w(sstatus_r() & ~SSTATUS_SIE);
}

/* Supervisor Interrupt Enable */
#define MIE_MEIE (1L << 11) /* external */
#define MIE_MTIE (1L << 7)  /* timer */
#define MIE_MSIE (1L << 3)  /* software */
static inline uint64 mie_r(void)
{
        uint64 r;
        asm volatile("csrr %0, mie" : "=r"(r) :);
        return r;
}

static inline void mie_w(uint64 v)
{
        asm volatile("csrw mie, %0" : : "r"(v));
}

static inline void stvec_w(uint64 v)
{
        asm volatile("csrw stvec, %0" : : "r"(v));
}

static inline void medeleg_w(uint64 v)
{
        asm volatile("csrw medeleg, %0" : : "r"(v));
}

static inline void mideleg_w(uint64 v)
{
        asm volatile("csrw mideleg, %0" : : "r"(v));
}

#endif

#endif