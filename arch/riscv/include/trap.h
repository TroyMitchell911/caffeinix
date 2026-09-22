/* S-mode trap-vector initialization and interrupt test accounting. */
#ifndef __CAFFEINIX_ARCH_RISCV_TRAP_H
#define __CAFFEINIX_ARCH_RISCV_TRAP_H

#include <riscv.h>

/**
 * trap_init() - Install the local kernel vector and enable IRQ source bits.
 *
 * Context:
 * Per-hart boot before scheduling; does not sleep.
 */
void trap_init(void);
/**
 * trap_init_lock() - Initialize global trap accounting before interrupts run.
 *
 * Context:
 * Boot hart single-threaded initialization; does not sleep.
 */
void trap_init_lock(void);
/**
 * trap_interrupt_count() - Return the relaxed aggregate interrupt count.
 *
 * Context:
 * Any context after trap_init_lock(); does not sleep.
 *
 * Return:
 * Monotonic but non-snapshot count of timer, external, and software IRQs.
 */
uint64 trap_interrupt_count(void);

#endif
