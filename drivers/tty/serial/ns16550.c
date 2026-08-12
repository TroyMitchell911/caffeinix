#include <device_model.h>
#include <io.h>
#include <mystring.h>
#include <ns16550.h>
#include <of.h>
#include <platform_device.h>
#include <resource.h>
#include <uart.h>

#define NS16550_MAX_PORTS TTY_MAX_DEVICES

#define UART_RX 0
#define UART_TX 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5

#define UART_IER_RX 0x01
#define UART_IER_TX 0x02
#define UART_FCR_ENABLE 0x01
#define UART_FCR_CLEAR 0x06
#define UART_LCR_8N1 0x03
#define UART_LCR_DLAB 0x80
#define UART_LSR_DATA_READY 0x01
#define UART_LSR_THRE 0x20

struct ns16550_device {
	struct uart_port port;
	struct platform_device *platform;
	int used;
};

static struct ns16550_device ns16550_ports[NS16550_MAX_PORTS];

static volatile void *ns16550_register(struct uart_port *port,
				       uint32 number)
{
	return (volatile uint8 *)port->membase +
	       ((uint64)number << port->reg_shift);
}

static uint8 ns16550_read(struct uart_port *port, uint32 number)
{
	volatile void *address = ns16550_register(port, number);

	return port->reg_io_width == 4 ? readl(address) : readb(address);
}

static void ns16550_write(struct uart_port *port, uint32 number,
			  uint8 value)
{
	volatile void *address = ns16550_register(port, number);

	if (port->reg_io_width == 4)
		writel(value, address);
	else
		writeb(value, address);
}

static int ns16550_startup(struct uart_port *port)
{
	uint32 divisor = port->clock / (16 * 38400);

	if (!divisor)
		divisor = 1;
	if (divisor > 0xffff)
		return DRIVER_ERR_INVAL;
	ns16550_write(port, UART_IER, 0);
	ns16550_write(port, UART_LCR, UART_LCR_DLAB);
	ns16550_write(port, UART_RX, divisor & 0xff);
	ns16550_write(port, UART_IER, divisor >> 8);
	ns16550_write(port, UART_LCR, UART_LCR_8N1);
	ns16550_write(port, UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR);
	return DRIVER_OK;
}

static void ns16550_shutdown(struct uart_port *port)
{
	ns16550_write(port, UART_IER, 0);
}

static int ns16550_tx_ready(struct uart_port *port)
{
	return ns16550_read(port, UART_LSR) & UART_LSR_THRE;
}

static void ns16550_put_char(struct uart_port *port, int character)
{
	ns16550_write(port, UART_TX, character);
}

static int ns16550_get_char(struct uart_port *port)
{
	if (!(ns16550_read(port, UART_LSR) & UART_LSR_DATA_READY))
		return -1;
	return ns16550_read(port, UART_RX);
}

static void ns16550_update_ier(struct uart_port *port, uint8 mask,
			       int enable)
{
	uint8 value = ns16550_read(port, UART_IER);

	value = enable ? value | mask : value & ~mask;
	ns16550_write(port, UART_IER, value);
}

static void ns16550_enable_rx_irq(struct uart_port *port, int enable)
{
	ns16550_update_ier(port, UART_IER_RX, enable);
}

static void ns16550_enable_tx_irq(struct uart_port *port, int enable)
{
	ns16550_update_ier(port, UART_IER_TX, enable);
}

static const struct uart_operations ns16550_operations = {
	.startup = ns16550_startup,
	.shutdown = ns16550_shutdown,
	.tx_ready = ns16550_tx_ready,
	.put_char = ns16550_put_char,
	.get_char = ns16550_get_char,
	.enable_rx_irq = ns16550_enable_rx_irq,
	.enable_tx_irq = ns16550_enable_tx_irq,
};

static struct ns16550_device *ns16550_alloc(void)
{
	int index;

	for (index = 0; index < NS16550_MAX_PORTS; index++) {
		if (ns16550_ports[index].used)
			continue;
		ns16550_ports[index].used = 1;
		return &ns16550_ports[index];
	}
	return 0;
}

static int ns16550_probe(struct platform_device *platform)
{
	struct ns16550_device *device;
	struct device_node *node = platform->device.of_node;
	struct resource *resource;
	uint32 value;
	int irq, status;

	resource = platform_get_resource(platform, RESOURCE_MEM, 0);
	irq = platform_get_irq(platform, 0);
	if (!resource || irq <= 0 ||
	    of_property_read_u32(node, "clock-frequency", &value) < 0)
		return DRIVER_ERR_NODEV;
	device = ns16550_alloc();
	if (!device)
		return DRIVER_ERR_BUSY;
	device->platform = platform;
	device->port.mapbase = resource->start;
	device->port.mapsize = resource_size(resource);
	device->port.irq = irq;
	device->port.clock = value;
	device->port.line = of_alias_get_id(node, "serial");
	device->port.reg_io_width = 1;
	device->port.operations = &ns16550_operations;
	device->port.of_node = node;
	device->port.private = device;
	if (of_property_read_u32(node, "reg-shift", &value) == 0)
		device->port.reg_shift = value;
	if (of_property_read_u32(node, "reg-io-width", &value) == 0)
		device->port.reg_io_width = value;
	if (device->port.reg_shift > 4 ||
	    (device->port.reg_io_width != 1 &&
	     device->port.reg_io_width != 4) ||
	    device->port.mapsize <
		((UART_LSR << device->port.reg_shift) +
		 device->port.reg_io_width)) {
		status = DRIVER_ERR_INVAL;
		goto release;
	}
	device->port.membase = ioremap(device->port.mapbase,
	                               device->port.mapsize);
	if (!device->port.membase) {
		status = DRIVER_ERR_NODEV;
		goto release;
	}
	status = uart_add_one_port(&device->port);
	if (status < 0)
		goto unmap;
	dev_set_drvdata(&platform->device, device);
	return DRIVER_OK;

unmap:
	iounmap(device->port.membase, device->port.mapsize);
release:
	memset(device, 0, sizeof(*device));
	return status;
}

static void ns16550_remove(struct platform_device *platform)
{
	struct ns16550_device *device = dev_get_drvdata(&platform->device);

	if (!device)
		return;
	uart_remove_one_port(&device->port);
	iounmap(device->port.membase, device->port.mapsize);
	dev_set_drvdata(&platform->device, 0);
	memset(device, 0, sizeof(*device));
}

static const struct of_device_id ns16550_matches[] = {
	{ .compatible = "ns16550a" },
	{ 0 },
};

static struct platform_driver ns16550_driver = {
	.driver = {
		.name = "ns16550",
	},
	.of_match_table = ns16550_matches,
	.probe = ns16550_probe,
	.remove = ns16550_remove,
};

int ns16550_init(void)
{
	return platform_driver_register(&ns16550_driver);
}
