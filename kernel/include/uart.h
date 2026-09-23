/* Common UART port layer shared by serial hardware drivers. */
#ifndef __CAFFEINIX_KERNEL_UART_H
#define __CAFFEINIX_KERNEL_UART_H

#include <console.h>
#include <spinlock.h>
#include <tty.h>
#include <typedefs.h>

#define UART_TX_BUFFER_SIZE 128
#define UART_RX_BREAK 0x100

struct device_node;
struct uart_port;

struct uart_operations {
	int (*startup)(struct uart_port *port);
	void (*shutdown)(struct uart_port *port);
	int (*tx_ready)(struct uart_port *port);
	void (*put_char)(struct uart_port *port, int character);
	int (*get_char)(struct uart_port *port);
	void (*enable_rx_irq)(struct uart_port *port, int enable);
	void (*enable_tx_irq)(struct uart_port *port, int enable);
};

struct uart_port {
	struct spinlock lock;
	struct wait_queue transmit_wait;
	void *membase;
	uint64 mapbase;
	uint64 mapsize;
	uint32 irq;
	uint32 clock;
	uint32 reg_shift;
	uint32 reg_io_width;
	int line;
	int registered;
	char transmit[UART_TX_BUFFER_SIZE];
	uint64 transmit_head;
	uint64 transmit_tail;
	const struct uart_operations *operations;
	struct device_node *of_node;
	void *private;
	struct tty tty;
	struct console console;
};

/**
 * uart_add_one_port() - Register one initialized UART port with TTY.
 * @port: Hardware driver-owned port with complete operations and line number.
 *
 * Context:
 * Process context; startup may access hardware and request an IRQ.
 * Return:
 * Zero on success or negative on startup, IRQ, or TTY failure.
 */
int uart_add_one_port(struct uart_port *port);
/**
 * uart_remove_one_port() - Stop and unregister a UART port.
 * @port: Registered port, or %NULL.
 *
 * Context:
 * Process context; disables IRQs before releasing TTY state.
 */
void uart_remove_one_port(struct uart_port *port);
/**
 * uart_handle_irq() - Service received and transmitted characters for a port.
 * @port: Registered UART port.
 *
 * Context:
 * IRQ context; holds the port lock around transmit state.
 * Return:
 * %IRQ_HANDLED when work was performed, otherwise %IRQ_NONE.
 */
int uart_handle_irq(struct uart_port *port);
/**
 * uart_poll_put_char() - Busy-wait and transmit one early-console character.
 * @port: Initialized UART port.
 * @character: Low eight bits to transmit.
 *
 * Context:
 * Atomic-safe; may busy-wait and does not use the transmit queue.
 */
void uart_poll_put_char(struct uart_port *port, int character);
/**
 * uart_core_selftest() - Exercise UART queue and interrupt mechanics.
 *
 * Context:
 * Test-only boot context with no live hardware port.
 * Return:
 * Zero on success, negative on failure.
 */
int uart_core_selftest(void);

#endif
