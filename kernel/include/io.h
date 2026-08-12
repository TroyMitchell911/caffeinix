#ifndef __CAFFEINIX_KERNEL_IO_H
#define __CAFFEINIX_KERNEL_IO_H

#include <typedefs.h>

void *ioremap(uint64 address, uint64 size);
void iounmap(void *address, uint64 size);

static inline uint8 readb(const volatile void *address)
{
	return *(const volatile uint8 *)address;
}

static inline void writeb(uint8 value, volatile void *address)
{
	*(volatile uint8 *)address = value;
}

static inline uint32 readl(const volatile void *address)
{
	return *(const volatile uint32 *)address;
}

static inline void writel(uint32 value, volatile void *address)
{
	*(volatile uint32 *)address = value;
}

#endif
