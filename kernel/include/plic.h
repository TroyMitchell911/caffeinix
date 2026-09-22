/* RISC-V Platform-Level Interrupt Controller interface. */
#ifndef __CAFFEINIX_KERNEL_PLIC_H
#define __CAFFEINIX_KERNEL_PLIC_H

#include <typedefs.h>
#include <mem_layout.h>

/**
 * plic_init() - Discover PLIC contexts and initialize source priorities.
 *
 * Context:
 * Boot process context after OF and CPU topology initialization.
 */
void plic_init(void);
/**
 * plic_init_hart() - Disable sources and allow supervisor interrupts on a hart.
 *
 * Context:
 * Per-hart boot context; does not sleep.
 */
void plic_init_hart(void);
/**
 * plic_enable() - Enable an IRQ source for the boot-hart context.
 * @irq: Valid non-zero PLIC source.
 *
 * Context:
 * Atomic-safe; current routing is deliberately boot-hart only.
 */
void plic_enable(uint32 irq);
/**
 * plic_disable() - Disable an IRQ source for the boot-hart context.
 * @irq: Valid non-zero PLIC source.
 *
 * Context:
 * Atomic-safe; does not synchronize an already running handler.
 */
void plic_disable(uint32 irq);
/**
 * plic_claim() - Claim the next pending IRQ for the current hart.
 *
 * Context:
 * Supervisor external-interrupt context.
 * Return:
 * Positive source number, or zero when no source is pending.
 */
int plic_claim(void);
/**
 * plic_complete() - Complete a source claimed by plic_claim().
 * @irq: Claimed IRQ source, or zero.
 *
 * Context:
 * Supervisor external-interrupt context; must follow dispatch.
 */
void plic_complete(int irq);

#endif
