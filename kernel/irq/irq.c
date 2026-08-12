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

void irq_init(void)
{
	spinlock_init(&irq_table.lock, "irq table");
}

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
