/* Exclusive external-IRQ handler registry between PLIC and device drivers. */
#include <irq.h>
#include <plic.h>
#include <spinlock.h>

struct irq_descriptor {
	irq_handler_t handler;
	void *data;
	const char *name;
};

static struct {
	struct spinlock lock;
	struct irq_descriptor descriptors[IRQ_MAX];
} irq_table;

/*
 * irq_init() - Initialize the exclusive handler table.
 *
 * Context:
 * Early boot before PLIC source registration; does not sleep.
 */
void irq_init(void)
{
	spinlock_init(&irq_table.lock, "irq table");
}

/*
 * request_irq() - Publish a handler before enabling its PLIC source.
 * @irq: Non-zero exclusive source.
 * @handler: Interrupt-context callback.
 * @data: Callback ownership token.
 * @name: Stable diagnostic name.
 *
 * Context:
 * Process context; internal lock only, no sleep.
 * Return:
 * Zero or negative invalid/busy error.
 */
int request_irq(uint32 irq, irq_handler_t handler, void *data,
		const char *name)
{
	struct irq_descriptor *descriptor;

	if (!irq || irq >= IRQ_MAX || !handler || !name)
		return -1;
	spinlock_acquire(&irq_table.lock);
	descriptor = &irq_table.descriptors[irq];
	if (descriptor->handler) {
		spinlock_release(&irq_table.lock);
		return -1;
	}
	descriptor->handler = handler;
	descriptor->data = data;
	descriptor->name = name;
	spinlock_release(&irq_table.lock);
	plic_enable(irq);
	return 0;
}

/*
 * free_irq() - Unpublish a matching handler then disable its PLIC source.
 * @irq: Exclusive source registered by caller.
 * @data: Exact callback token.
 *
 * Context:
 * Process context; caller synchronizes active handler lifetime.
 * Return:
 * Zero or negative invalid/mismatch error.
 */
int free_irq(uint32 irq, void *data)
{
	struct irq_descriptor *descriptor;

	if (!irq || irq >= IRQ_MAX)
		return -1;
	spinlock_acquire(&irq_table.lock);
	descriptor = &irq_table.descriptors[irq];
	if (!descriptor->handler || descriptor->data != data) {
		spinlock_release(&irq_table.lock);
		return -1;
	}
	descriptor->handler = 0;
	descriptor->data = 0;
	descriptor->name = 0;
	spinlock_release(&irq_table.lock);
	plic_disable(irq);
	return 0;
}

/*
 * irq_dispatch() - Call the snapshotted handler for a claimed source.
 * @irq: PLIC claimed source.
 *
 * Context:
 * Interrupt context; handler execution occurs outside table lock.
 * Return:
 * Handler status or %IRQ_NONE.
 */
int irq_dispatch(uint32 irq)
{
	irq_handler_t handler;
	void *data;

	if (!irq || irq >= IRQ_MAX)
		return IRQ_NONE;
	spinlock_acquire(&irq_table.lock);
	handler = irq_table.descriptors[irq].handler;
	data = irq_table.descriptors[irq].data;
	spinlock_release(&irq_table.lock);
	return handler ? handler(irq, data) : IRQ_NONE;
}
