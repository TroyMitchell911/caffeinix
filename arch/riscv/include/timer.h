/* Device-tree timebase and per-hart supervisor timer control. */
#ifndef __CAFFEINIX_ARCH_RISCV_TIMER_H
#define __CAFFEINIX_ARCH_RISCV_TIMER_H

#include <typedefs.h>

/**
 * timer_early_init() - Read and validate the FDT timebase frequency.
 *
 * Context:
 * Boot hart before allocation; does not sleep and panics for invalid DT data.
 */
void timer_early_init(void);
/**
 * timer_init() - Allocate per-CPU timer state after topology discovery.
 *
 * Context:
 * Single-threaded boot after timer_early_init(); may allocate but does not
 * enable any hart timer.
 */
void timer_init(void);
/**
 * timer_init_hart() - Arm the current hart's initial SBI timer deadline.
 *
 * Context:
 * Per-hart boot context after timer_init(); does not sleep and panics on SBI
 * programming failure.
 */
void timer_init_hart(void);
/**
 * timer_wait_for_interrupt() - Verify that the local timer can interrupt.
 *
 * Context:
 * Per-hart boot; temporarily enables interrupts and busy-waits, so it must
 * not run while holding locks.
 */
void timer_wait_for_interrupt(void);
/**
 * timer_interrupt() - Account for and rearm the current hart's timer tick.
 *
 * Context:
 * Local supervisor timer-interrupt context; does not sleep.
 */
void timer_interrupt(void);
/**
 * timer_set_active() - Select the active scheduling tick cadence locally.
 *
 * Context:
 * Local scheduler context; does not sleep.
 */
void timer_set_active(void);
/**
 * timer_set_idle() - Select the sparse idle tick cadence locally.
 *
 * Logical CPU 0 keeps the active tick to expire global timed waits. Only
 * other CPUs switch to the idle interval.
 *
 * Context:
 * Local scheduler context; does not sleep.
 */
void timer_set_idle(void);
/**
 * timer_frequency() - Return the validated FDT timebase frequency.
 *
 * Context:
 * Any context after timer_early_init(); does not sleep.
 *
 * Return:
 * Timebase ticks per second.
 */
uint64 timer_frequency(void);

#endif
