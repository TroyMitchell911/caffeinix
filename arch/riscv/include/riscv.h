/* RISC-V SV39 constants and small S-mode CSR/interrupt access primitives. */
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
#define PTE_A                   (1L << 6)
#define PTE_D                   (1L << 7)
/* RSW bit used to retain ownership while PTE_U is clear for PROT_NONE. */
#define PTE_SW_USER             (1L << 8)
/* RSW bit used for writable private mappings shared until first write. */
#define PTE_SW_COW              (1L << 9)

#define SCAUSE_INSTRUCTION_PAGE_FAULT 12
#define SCAUSE_LOAD_PAGE_FAULT        13
#define SCAUSE_STORE_PAGE_FAULT       15

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

/**
 * satp_w() - Install a supervisor address-translation control value.
 * @v: Complete SATP value, normally built with MAKE_SATP().
 *
 * Context:
 * Local S-mode execution with interrupts controlled by the caller.
 * Callers issue sfence_vma() when translations from the old value may persist.
 */
static inline void satp_w(uint64 v)
{
        asm volatile("csrw satp, %0" : : "r"(v));
}

/**
 * satp_r() - Read the supervisor address-translation control register.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current SATP CSR value.
 */
static inline uint64 satp_r(void)
{
        uint64 s;
        asm volatile("csrr %0, satp" : "=r"(s) :);
        return s;
}

/**
 * sfence_vma() - Flush local address translations for every address and ASID.
 *
 * Context:
 * Local S-mode context. Remote harts require an SBI RFENCE request.
 */
static inline void sfence_vma(void)
{
        asm volatile("sfence.vma zero, zero");
}

/**
 * fence_i() - Synchronize local instruction fetch with prior code writes.
 *
 * Context:
 * Local S-mode context. Remote harts require SBI remote_fence_i().
 */
static inline void fence_i(void)
{
	asm volatile("fence.i" : : : "memory");
}

/* Supervisor Interrupt Enable */
#define SIE_SEIE (1L << 9)  /* external */
#define SIE_STIE (1L << 5)  /* timer */
#define SIE_SSIE (1L << 1)  /* software */
/**
 * sie_r() - Read supervisor interrupt-enable bits.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current SIE CSR value.
 */
static inline uint64 sie_r(void)
{
        uint64 r;
        asm volatile("csrr %0, sie" : "=r"(r) :);
        return r;
}

/**
 * sie_w() - Replace supervisor interrupt-enable bits.
 * @v: Complete SIE CSR value to install.
 *
 * Context:
 * Local S-mode context. Does not sleep.
 */
static inline void sie_w(uint64 v)
{
        asm volatile("csrw sie, %0" : : "r"(v));
}

#define SIP_SSIP (1L << 1)
/**
 * sip_clear_ssip() - Acknowledge the local supervisor software interrupt.
 *
 * Context:
 * Local S-mode trap context. Does not sleep.
 */
static inline void sip_clear_ssip(void)
{
	asm volatile("csrc sip, %0" : : "r"(SIP_SSIP) : "memory");
}

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_FS_MASK (3L << 13)
#define SSTATUS_FS_DIRTY (3L << 13)
#define SSTATUS_UPIE (1L << 4) // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)  // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)  // User Interrupt Enable

/**
 * sstatus_r() - Read supervisor status.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current SSTATUS CSR value.
 */
static inline uint64 sstatus_r()
{
        uint64 v;
        asm volatile("csrr %0, sstatus" : "=r" (v) );
        return v;
}

/**
 * sstatus_w() - Replace supervisor status.
 * @v: Complete SSTATUS CSR value to install.
 *
 * Context:
 * Local S-mode context. Does not sleep.
 */
static inline void sstatus_w(uint64 v)
{
  asm volatile("csrw sstatus, %0" : : "r" (v));
}

/**
 * scause_r() - Read the current supervisor trap cause.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current SCAUSE CSR value.
 */
static inline uint64 scause_r(void)
{
        uint64 r;
        asm volatile("csrr %0, scause" : "=r"(r) :);
        return r;
}

/**
 * stval_r() - Read the current trap fault value.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current STVAL CSR value.
 */
static inline uint64 stval_r(void)
{
        uint64 r;
        asm volatile("csrr %0, stval" : "=r"(r) :);
        return r;
}

/**
 * sepc_r() - Read the saved supervisor trap return PC.
 *
 * Context:
 * Any S-mode context. Does not sleep.
 * Return:
 * Current SEPC CSR value.
 */
static inline uint64 sepc_r(void)
{
        uint64 r;
        asm volatile("csrr %0, sepc" : "=r"(r) :);
        return r;
}

/**
 * sepc_w() - Set the PC used by the next sret.
 * @v: Aligned supervisor return PC.
 *
 * Context:
 * Local S-mode trap setup. Does not sleep.
 */
static inline void sepc_w(uint64 v)
{
  asm volatile("csrw sepc, %0" : : "r" (v));
}


/**
 * tp_w() - Set the Caffeinix logical CPU ID held in tp.
 * @v: Dense logical CPU ID for this hart.
 *
 * Context:
 * Local early S-mode setup. Does not sleep.
 */
static inline void tp_w(uint64 v)
{
        asm volatile("mv tp, %0" : : "r"(v));
}

/**
 * tp_r() - Read the current logical CPU ID from tp.
 *
 * Context:
 * Any local S-mode context. Does not sleep.
 * Return:
 * Dense logical CPU ID installed by early boot.
 */
static inline uint64 tp_r(void)
{
        uint64 r;
        asm volatile("mv %0, tp" : "=r"(r) :);
        return r;
}

/**
 * intr_status() - Test whether local supervisor interrupts are enabled.
 *
 * Context:
 * Any local S-mode context. Does not sleep.
 * Return:
 * Nonzero if SSTATUS_SIE is set, otherwise zero.
 */
static inline uint64 intr_status(void)
{
        uint64 x = sstatus_r();
        return (x & SSTATUS_SIE) != 0;
}

/**
 * intr_on() - Enable supervisor interrupts on this hart.
 *
 * Context:
 * Local S-mode context. Does not sleep.
 */
static inline void intr_on(void)
{
        sstatus_w(sstatus_r() | SSTATUS_SIE);
}

/**
 * intr_off() - Disable supervisor interrupts on this hart.
 *
 * Context:
 * Local S-mode context. Does not sleep.
 */
static inline void intr_off(void)
{
        sstatus_w(sstatus_r() & ~SSTATUS_SIE);
}

/**
 * intr_save() - Disable local interrupts and save supervisor status.
 *
 * Context:
 * Local S-mode context. Does not sleep.
 * Return:
 * Prior complete SSTATUS value for intr_restore().
 */
static inline uint64 intr_save(void)
{
	uint64 status;

	asm volatile("csrrc %0, sstatus, %1"
		     : "=r"(status)
		     : "r"(SSTATUS_SIE)
		     : "memory");
	return status;
}

/**
 * intr_restore() - Restore local interrupt enable state.
 * @status: Prior SSTATUS value returned by intr_save().
 *
 * Context:
 * Local S-mode context. Does not sleep.
 */
static inline void intr_restore(uint64 status)
{
	if (status & SSTATUS_SIE)
		asm volatile("csrs sstatus, %0"
			     :
			     : "r"(SSTATUS_SIE)
			     : "memory");
	else
		asm volatile("csrc sstatus, %0"
			     :
			     : "r"(SSTATUS_SIE)
			     : "memory");
}

/**
 * stvec_w() - Set the supervisor trap-vector address.
 * @v: Aligned address and mode bits for STVEC.
 *
 * Context:
 * Local S-mode trap setup. Does not sleep.
 */
static inline void stvec_w(uint64 v)
{
        asm volatile("csrw stvec, %0" : : "r"(v));
}

/**
 * time_r() - Read the platform timebase counter.
 *
 * Context:
 * Any local S-mode context. Does not sleep.
 * Return:
 * Current timebase value in platform ticks.
 */
static inline uint64 time_r(void)
{
	uint64 value;

	asm volatile("rdtime %0" : "=r"(value));
	return value;
}

/**
 * wait_for_interrupt() - Wait until a pending interrupt resumes this hart.
 *
 * Context:
 * Local S-mode idle context with interrupts arranged by the caller.
 * Does not sleep in the scheduler sense.
 */
static inline void wait_for_interrupt(void)
{
	asm volatile("wfi" : : : "memory");
}

#endif

#endif
