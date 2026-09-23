/*
 * Memory-mapped I/O accessors.
 *
 * ioremap() establishes missing identity mappings for platform resources.
 * Mapping setup is caller-serialized; these mappings persist after iounmap().
 */
#ifndef __CAFFEINIX_KERNEL_IO_H
#define __CAFFEINIX_KERNEL_IO_H

#include <typedefs.h>

/**
 * ioremap() - Establish an identity-mapped kernel MMIO range.
 * @address: Physical MMIO base.
 * @size: Nonzero byte length wholly below MAXVA with @address.
 *
 * Context: Serialized driver or early-boot setup after kvm_create(), not
 * interrupt context. May allocate intermediate page tables. The caller
 * serializes kernel page-table updates and accesses only the requested range.
 * Only the local hart's translations are flushed. Existing mappings are kept;
 * a failed call leaves any earlier mappings and allocated tables in place.
 * Return: Identity-mapped address, or %NULL on invalid range or mapping
 * failure.
 */
void *ioremap(uint64 address, uint64 size);
/**
 * iounmap() - Finish using an identity-mapped MMIO range.
 * @address: Address returned by ioremap().
 * @size: Same byte length passed to ioremap().
 *
 * Context: Any context; a no-op. No reference is tracked, and neither mappings
 * nor page tables are removed or freed.
 */
void iounmap(void *address, uint64 size);

/**
 * readb() - Read one byte from an MMIO register.
 * @address: Valid volatile register address.
 *
 * Context:
 * Atomic-safe; ordering beyond the volatile access is caller-owned.
 * Return:
 * Register value.
 */
static inline uint8 readb(const volatile void *address)
{
	return *(const volatile uint8 *)address;
}

/**
 * writeb() - Write one byte to an MMIO register.
 * @value: Byte to store.
 * @address: Valid volatile register address.
 *
 * Context:
 * Atomic-safe; callers add device barriers where required.
 */
static inline void writeb(uint8 value, volatile void *address)
{
	*(volatile uint8 *)address = value;
}

/**
 * readl() - Read one 32-bit value from an MMIO register.
 * @address: Four-byte aligned volatile register address.
 *
 * Context:
 * Atomic-safe; caller owns device-specific ordering.
 * Return:
 * Register value.
 */
static inline uint32 readl(const volatile void *address)
{
	return *(const volatile uint32 *)address;
}

/**
 * writel() - Write one 32-bit value to an MMIO register.
 * @value: Value to store.
 * @address: Four-byte aligned volatile register address.
 *
 * Context:
 * Atomic-safe; caller owns device-specific ordering.
 */
static inline void writel(uint32 value, volatile void *address)
{
	*(volatile uint32 *)address = value;
}

#endif
