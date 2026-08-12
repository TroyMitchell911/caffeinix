#ifndef __CAFFEINIX_KERNEL_IRQ_H
#define __CAFFEINIX_KERNEL_IRQ_H

#include <typedefs.h>

#define IRQ_MAX 96

#define IRQ_NONE 0
#define IRQ_HANDLED 1

typedef int (*irq_handler_t)(uint32 irq, void *data);

void irq_init(void);
int request_irq(uint32 irq, irq_handler_t handler, void *data,
		const char *name);
int free_irq(uint32 irq, void *data);
int irq_dispatch(uint32 irq);

#endif
