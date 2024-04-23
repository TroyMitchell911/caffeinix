#include <plic.h>
#include <scheduler.h>

void plic_init(void)
{
        *(uint32*)(PLIC + UART0_IRQ*4) = 1;
        *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
}

void plic_init_hart(void)
{
        int hart = cpuid();
  
        /* set enable bits for this hart's S-mode for the uart and virtio disk. */
        *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

        /* set this hart's S-mode priority threshold to 0. */
        *(uint32*)PLIC_SPRIORITY(hart) = 0;
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
