#include <console.h>
#include <char_device.h>
#include <device_model.h>
#include <irq.h>
#include <mystring.h>
#include <process.h>
#include <spinlock.h>
#include <tty.h>
#include <uart.h>

static void uart_start_transmit_locked(struct uart_port *port)
{
	while (port->transmit_tail != port->transmit_head &&
	       port->operations->tx_ready(port)) {
		int character =
			port->transmit[port->transmit_tail %
			               UART_TX_BUFFER_SIZE];

		port->operations->put_char(port, character);
		port->transmit_tail++;
		wakeup_(&port->transmit_tail);
	}
	port->operations->enable_tx_irq(
		port, port->transmit_tail != port->transmit_head);
}

static int64 uart_tty_write(struct tty *tty, const char *buffer,
			    uint64 count)
{
	struct uart_port *port = tty->driver_data;
	uint64 written = 0;

	if (!port || !port->registered)
		return VFS_ERR_NODEV;
	spinlock_acquire(&port->lock);
	while (written < count) {
		while (port->transmit_head - port->transmit_tail ==
		       UART_TX_BUFFER_SIZE)
			sleep_(&port->transmit_tail, &port->lock);
		port->transmit[port->transmit_head % UART_TX_BUFFER_SIZE] =
			buffer[written++];
		port->transmit_head++;
		uart_start_transmit_locked(port);
	}
	spinlock_release(&port->lock);
	return written;
}

void uart_poll_put_char(struct uart_port *port, int character)
{
	if (!port || !port->operations)
		return;
	while (!port->operations->tx_ready(port))
		;
	port->operations->put_char(port, character);
}

static void uart_tty_put_char(struct tty *tty, int character)
{
	uart_poll_put_char(tty->driver_data, character);
}

static const struct tty_operations uart_tty_operations = {
	.write = uart_tty_write,
	.put_char = uart_tty_put_char,
};

static void uart_console_put_char(struct console *console, int character)
{
	uart_poll_put_char(console->private, character);
}

static const struct console_operations uart_console_operations = {
	.put_char = uart_console_put_char,
};

int uart_handle_irq(struct uart_port *port)
{
	int character;

	if (!port || !port->registered)
		return IRQ_NONE;
	while ((character = port->operations->get_char(port)) >= 0)
		tty_receive_char(&port->tty, character);
	spinlock_acquire(&port->lock);
	uart_start_transmit_locked(port);
	spinlock_release(&port->lock);
	return IRQ_HANDLED;
}

static int uart_interrupt(uint32 irq, void *data)
{
	struct uart_port *port = data;

	if (!port || port->irq != irq)
		return IRQ_NONE;
	return uart_handle_irq(port);
}

int uart_add_one_port(struct uart_port *port)
{
	int status;

	if (!port || port->registered || !port->membase || !port->mapsize ||
	    !port->irq || !port->operations || !port->operations->startup ||
	    !port->operations->shutdown || !port->operations->tx_ready ||
	    !port->operations->put_char || !port->operations->get_char ||
	    !port->operations->enable_rx_irq ||
	    !port->operations->enable_tx_irq)
		return DRIVER_ERR_INVAL;
	spinlock_init(&port->lock, "UART port");
	port->transmit_head = 0;
	port->transmit_tail = 0;
	status = port->operations->startup(port);
	if (status < 0)
		return status;
	status = tty_register(&port->tty, "ttyS", port->line,
	                      &uart_tty_operations, port);
	if (status < 0)
		goto shutdown;
	port->line = port->tty.line;
	port->registered = 1;
	status = request_irq(port->irq, uart_interrupt, port,
	                     port->tty.name);
	if (status < 0)
		goto unregister_tty;
	port->operations->enable_rx_irq(port, 1);
	port->console.name = port->tty.name;
	port->console.operations = &uart_console_operations;
	port->console.of_node = port->of_node;
	port->console.tty = &port->tty;
	port->console.private = port;
	status = console_register(&port->console);
	if (status < 0)
		goto free_interrupt;
	return DRIVER_OK;

free_interrupt:
	port->operations->enable_rx_irq(port, 0);
	free_irq(port->irq, port);
unregister_tty:
	port->registered = 0;
	tty_unregister(&port->tty);
shutdown:
	port->operations->shutdown(port);
	return status;
}

void uart_remove_one_port(struct uart_port *port)
{
	if (!port || !port->registered)
		return;
	console_unregister(&port->console);
	port->operations->enable_rx_irq(port, 0);
	port->operations->enable_tx_irq(port, 0);
	free_irq(port->irq, port);
	port->registered = 0;
	tty_unregister(&port->tty);
	port->operations->shutdown(port);
}

struct uart_test_state {
	char output[32];
	int output_count;
	int receive;
	int receive_pending;
	int receive_irq;
	int transmit_irq;
};

static int uart_test_startup(struct uart_port *port)
{
	(void)port;
	return DRIVER_OK;
}

static void uart_test_shutdown(struct uart_port *port)
{
	(void)port;
}

static int uart_test_tx_ready(struct uart_port *port)
{
	(void)port;
	return 1;
}

static void uart_test_put_char(struct uart_port *port, int character)
{
	struct uart_test_state *state = port->private;

	if (state->output_count < sizeof(state->output))
		state->output[state->output_count++] = character;
}

static int uart_test_get_char(struct uart_port *port)
{
	struct uart_test_state *state = port->private;

	if (!state->receive_pending)
		return -1;
	state->receive_pending = 0;
	return state->receive;
}

static void uart_test_rx_irq(struct uart_port *port, int enable)
{
	struct uart_test_state *state = port->private;

	state->receive_irq = enable;
}

static void uart_test_tx_irq(struct uart_port *port, int enable)
{
	struct uart_test_state *state = port->private;

	state->transmit_irq = enable;
}

static const struct uart_operations uart_test_operations = {
	.startup = uart_test_startup,
	.shutdown = uart_test_shutdown,
	.tx_ready = uart_test_tx_ready,
	.put_char = uart_test_put_char,
	.get_char = uart_test_get_char,
	.enable_rx_irq = uart_test_rx_irq,
	.enable_tx_irq = uart_test_tx_irq,
};

int uart_core_selftest(void)
{
	static struct uart_port ports[2];
	static struct uart_test_state states[2];
	struct char_device_node node;
	int first_line, second_line;

	memset(ports, 0, sizeof(ports));
	memset(states, 0, sizeof(states));
	for (int index = 0; index < 2; index++) {
		ports[index].membase = &states[index];
		ports[index].mapbase = (uint64)&states[index];
		ports[index].mapsize = sizeof(states[index]);
		ports[index].irq = IRQ_MAX - 1 - index;
		ports[index].line = -1;
		ports[index].operations = &uart_test_operations;
		ports[index].private = &states[index];
		if (uart_add_one_port(&ports[index]) < 0)
			goto fail;
	}
	first_line = ports[0].line;
	second_line = ports[1].line;
	if (first_line == second_line || tty_get(first_line) != &ports[0].tty ||
	    tty_get(second_line) != &ports[1].tty ||
	    char_device_node_find(ports[0].tty.name, &node) < 0 ||
	    VFS_DEVICE_MINOR(node.device) != 64 + first_line ||
	    !states[0].receive_irq || !states[1].receive_irq)
		goto fail;
	if (ports[0].tty.operations->write(&ports[0].tty, "one", 3) != 3 ||
	    ports[1].tty.operations->write(&ports[1].tty, "two", 3) != 3 ||
	    states[0].output_count != 3 || states[1].output_count != 3 ||
	    memcmp(states[0].output, "one", 3) ||
	    memcmp(states[1].output, "two", 3))
		goto fail;
	states[0].receive = '\n';
	states[0].receive_pending = 1;
	if (uart_handle_irq(&ports[0]) != IRQ_HANDLED ||
	    ports[0].tty.commit_position != 1 ||
	    ports[1].tty.commit_position != 0)
		goto fail;
	uart_remove_one_port(&ports[1]);
	uart_remove_one_port(&ports[0]);
	if (tty_get(first_line) || tty_get(second_line) ||
	    char_device_node_find(ports[0].tty.name, &node) != VFS_ERR_NOENT)
		return -1;
	return 0;

fail:
	uart_remove_one_port(&ports[1]);
	uart_remove_one_port(&ports[0]);
	return -1;
}
