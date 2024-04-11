#ifndef __CAFFEINIX_ARCH_RISCV_RISCV_H
#define __CAFFEINIX_ARCH_RISCV_RISCV_H

#include <typedefs.h>

/* Previous mode of machine mode */
#define MSTATUS_MPP_MASK (3L << 11) 
/* Machine mode */
#define MSTATUS_MPP_M (3L << 11)
/* Supervisor mode */
#define MSTATUS_MPP_S (1L << 11)
/* User mode */
#define MSTATUS_MPP_U (0L << 11)

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

#endif