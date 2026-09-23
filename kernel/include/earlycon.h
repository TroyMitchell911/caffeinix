/* Early polling console used before normal serial drivers and IRQs exist. */
#ifndef __CAFFEINIX_KERNEL_EARLYCON_H
#define __CAFFEINIX_KERNEL_EARLYCON_H

#include <typedefs.h>

/**
 * earlycon_init() - Discover and initialize the boot stdout polling console.
 *
 * Uses the QEMU virt NS16550 fallback if the DT stdout node is unsuitable.
 *
 * Context:
 * Earliest boot; does not sleep and must precede normal console use.
 */
void earlycon_init(void);
/**
 * earlycon_putc() - Emit one character through the polling console.
 * @character: Low eight bits to transmit.
 *
 * Context:
 * Atomic-safe before teardown; may busy-wait for transmitter space.
 */
void earlycon_putc(int character);
/**
 * earlycon_address() - Return the selected UART physical base.
 *
 * Context:
 * Any context after earlycon_init().
 * Return:
 * DT-selected physical address, or the 0x10000000 fallback address when
 * stdout discovery/configuration fails.
 */
uint64 earlycon_address(void);
/**
 * earlycon_size() - Return the selected UART MMIO span.
 *
 * Context:
 * Any context after earlycon_init().
 * Return:
 * DT-selected span in bytes, or the 0x100-byte fallback span when stdout
 * discovery/configuration fails.
 */
uint64 earlycon_size(void);

#endif
