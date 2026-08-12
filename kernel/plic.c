#include <plic.h>
#include <irq.h>
#include <scheduler.h>

void plic_init(void)
{
	int irq;

	for (irq = 1; irq < IRQ_MAX; irq++)
		*(uint32 *)(PLIC + irq * 4) = 0;
}

void plic_init_hart(void)
{
        int hart = cpuid();
  
	for (int word = 0; word < (IRQ_MAX + 31) / 32; word++)
		*(uint32 *)(PLIC_SENABLE(hart) + word * sizeof(uint32)) = 0;

        /* set this hart's S-mode priority threshold to 0. */
        *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

void plic_enable(uint32 irq)
{
	uint32 *enable;

	if (!irq || irq >= IRQ_MAX)
		return;
	*(uint32 *)(PLIC + irq * 4) = 1;
	enable = (uint32 *)(PLIC_SENABLE(0) + irq / 32 * sizeof(uint32));
	__sync_fetch_and_or(enable, 1U << (irq % 32));
}

void plic_disable(uint32 irq)
{
	uint32 *enable;

	if (!irq || irq >= IRQ_MAX)
		return;
	enable = (uint32 *)(PLIC_SENABLE(0) + irq / 32 * sizeof(uint32));
	__sync_fetch_and_and(enable, ~(1U << (irq % 32)));
	*(uint32 *)(PLIC + irq * 4) = 0;
}

/* ask the PLIC what interrupt we should serve. */
int plic_claim(void)
{
        int hart = cpuid();
        int irq = *(uint32*)PLIC_SCLAIM(hart);
        return irq;
}

/* tell the PLIC we've served this IRQ. */
void plic_complete(int irq)
{
        int hart = cpuid();
        *(uint32*)PLIC_SCLAIM(hart) = irq;
}
