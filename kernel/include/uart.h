#ifndef __CAFFEINIX_KERNEL_UART_H
#define __CAFFEINIX_KERNEL_UART_H

#include <console.h>
#include <spinlock.h>
#include <tty.h>
#include <typedefs.h>

#define UART_TX_BUFFER_SIZE 128

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

int uart_add_one_port(struct uart_port *port);
void uart_remove_one_port(struct uart_port *port);
int uart_handle_irq(struct uart_port *port);
void uart_poll_put_char(struct uart_port *port, int character);
int uart_core_selftest(void);

#endif
