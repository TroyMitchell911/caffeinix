/* NS16550 platform-driver registration interface. */
#ifndef __CAFFEINIX_KERNEL_NS16550_H
#define __CAFFEINIX_KERNEL_NS16550_H

/**
 * ns16550_init() - Register the normal-operation NS16550 platform driver.
 *
 * Context:
 * Boot process context; registration may invoke probe.
 * Return:
 * Zero on success or a driver registration error.
 */
int ns16550_init(void);

#endif
