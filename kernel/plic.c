#include <plic.h>
#include <cpu.h>
#include <debug.h>
#include <irq.h>
#include <of.h>
#include <palloc.h>
#include <scheduler.h>

#define RISCV_IRQ_S_EXT 9

static int *plic_contexts;

static struct device_node *plic_node(void)
{
	struct device_node *node = 0;

	while ((node = of_next_node(node))) {
		if (of_device_is_available(node) &&
		    (of_device_is_compatible(node, "sifive,plic-1.0.0") ||
		     of_device_is_compatible(node, "riscv,plic0")))
			return node;
	}
	return 0;
}

static uint32 cpu_interrupt_phandle(int logical)
{
	struct device_node *cpu = cpu_of_node(logical);
	struct device_node *node = 0;

	while ((node = of_next_node(node))) {
		if (node->parent == cpu &&
		    of_device_is_compatible(node, "riscv,cpu-intc"))
			return of_node_phandle(node);
	}
	return 0;
}

static int plic_context_for_cpu(struct device_node *plic, int logical)
{
	uint32 cells, interrupt, parent, wanted;
	int context = 0, count, index = 0;

	wanted = cpu_interrupt_phandle(logical);
	count = of_property_count_u32(plic, "interrupts-extended");
	if (!wanted || count < 0)
		return -1;
	while (index < count) {
		struct device_node *controller;

		if (of_property_read_u32_index(plic, "interrupts-extended",
		                               index++, &parent) < 0)
			return -1;
		controller = of_find_node_by_phandle(parent);
		if (!controller ||
		    of_property_read_u32(controller, "#interrupt-cells",
		                         &cells) < 0 ||
		    cells != 1 || index + cells > count ||
		    of_property_read_u32_index(plic, "interrupts-extended",
		                               index, &interrupt) < 0)
			return -1;
		if (parent == wanted && interrupt == RISCV_IRQ_S_EXT)
			return context;
		index += cells;
		context++;
	}
	return -1;
}

static int current_plic_context(void)
{
	int logical = cpuid();

	if (logical < 0 || logical >= cpu_count())
		PANIC("invalid PLIC CPU");
	return plic_contexts[logical];
}

void plic_init(void)
{
	struct device_node *node = plic_node();
	int irq;
	int logical;

	if (!node)
		PANIC("missing PLIC node");
	plic_contexts = calloc(cpu_count(), sizeof(*plic_contexts));
	if (!plic_contexts)
		PANIC("allocate PLIC CPU contexts");
	for (logical = 0; logical < cpu_count(); logical++) {
		plic_contexts[logical] = plic_context_for_cpu(node, logical);
		if (plic_contexts[logical] < 0)
			PANIC("missing PLIC CPU context");
	}

	for (irq = 1; irq < IRQ_MAX; irq++)
		*(uint32 *)(PLIC + irq * 4) = 0;
}

void plic_init_hart(void)
{
	int context = current_plic_context();
  
	for (int word = 0; word < (IRQ_MAX + 31) / 32; word++)
		*(uint32 *)(PLIC_ENABLE(context) + word * sizeof(uint32)) = 0;

        /* set this hart's S-mode priority threshold to 0. */
	*(uint32 *)PLIC_THRESHOLD(context) = 0;
}

void plic_enable(uint32 irq)
{
	uint32 *enable;

	if (!irq || irq >= IRQ_MAX)
		return;
	*(uint32 *)(PLIC + irq * 4) = 1;
	enable = (uint32 *)(PLIC_ENABLE(plic_contexts[0]) +
	                    irq / 32 * sizeof(uint32));
	__sync_fetch_and_or(enable, 1U << (irq % 32));
}

void plic_disable(uint32 irq)
{
	uint32 *enable;

	if (!irq || irq >= IRQ_MAX)
		return;
	enable = (uint32 *)(PLIC_ENABLE(plic_contexts[0]) +
	                    irq / 32 * sizeof(uint32));
	__sync_fetch_and_and(enable, ~(1U << (irq % 32)));
	*(uint32 *)(PLIC + irq * 4) = 0;
}

/* ask the PLIC what interrupt we should serve. */
int plic_claim(void)
{
	int context = current_plic_context();
	int irq = *(uint32 *)PLIC_CLAIM(context);
        return irq;
}

/* tell the PLIC we've served this IRQ. */
void plic_complete(int irq)
{
	int context = current_plic_context();

	*(uint32 *)PLIC_CLAIM(context) = irq;
}
