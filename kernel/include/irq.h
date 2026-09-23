/*
 * Caffeinix external interrupt registration and dispatch interface.
 *
 * IRQ lines have one non-shared handler.  The PLIC transport owns delivery;
 * handlers run in interrupt context and must not sleep.
 */
#ifndef __CAFFEINIX_KERNEL_IRQ_H
#define __CAFFEINIX_KERNEL_IRQ_H

#include <typedefs.h>

#define IRQ_MAX 96

#define IRQ_NONE 0
#define IRQ_HANDLED 1

typedef int (*irq_handler_t)(uint32 irq, void *data);

/**
 * irq_init() - Initialize the IRQ descriptor table.
 *
 * Context:
 * Early boot before handlers are registered; does not sleep.
 */
void irq_init(void);
/**
 * request_irq() - Bind an exclusive handler to an external IRQ line.
 * @irq: Non-zero PLIC source below %IRQ_MAX.
 * @handler: Non-NULL interrupt-context callback.
 * @data: Opaque callback data, matched by free_irq().
 * @name: Stable diagnostic name.
 *
 * Context:
 * Process context; does not sleep.  Enables the source after publish.
 * Return:
 * Zero on success or negative for invalid or occupied lines.
 */
int request_irq(uint32 irq, irq_handler_t handler, void *data,
		const char *name);
/**
 * free_irq() - Disable and unbind an exclusive IRQ handler.
 * @irq: IRQ previously registered by the caller.
 * @data: Exact data pointer passed to request_irq().
 *
 * Context:
 * Process context; caller must ensure no active handler uses @data.
 * Return:
 * Zero on success or negative for a mismatched registration.
 */
int free_irq(uint32 irq, void *data);
/**
 * irq_dispatch() - Invoke the handler registered for one claimed IRQ.
 * @irq: Claimed PLIC source number.
 *
 * Context:
 * Interrupt context; handler must not sleep.
 * Return:
 * Handler result, or %IRQ_NONE for an invalid or unhandled source.
 */
int irq_dispatch(uint32 irq);

#endif
