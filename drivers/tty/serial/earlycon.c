#include <earlycon.h>
#include <of.h>
#include <resource.h>

#define EARLYCON_FALLBACK_ADDRESS 0x10000000UL
#define EARLYCON_FALLBACK_SIZE 0x100
#define EARLYCON_FALLBACK_CLOCK 3686400

#define UART_RX 0
#define UART_TX 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5

#define UART_FCR_ENABLE 0x01
#define UART_FCR_CLEAR 0x06
#define UART_LCR_8N1 0x03
#define UART_LCR_DLAB 0x80
#define UART_LSR_THRE 0x20

static struct {
	uint64 address;
	uint64 size;
	uint32 shift;
	uint32 width;
} early_console;

static volatile void *earlycon_register(uint32 number)
{
	return (volatile void *)(early_console.address +
	                         ((uint64)number << early_console.shift));
}

static uint8 earlycon_read(uint32 number)
{
	volatile void *address = earlycon_register(number);

	return early_console.width == 4 ?
		*(volatile uint32 *)address : *(volatile uint8 *)address;
}

static void earlycon_write(uint32 number, uint8 value)
{
	volatile void *address = earlycon_register(number);

	if (early_console.width == 4)
		*(volatile uint32 *)address = value;
	else
		*(volatile uint8 *)address = value;
}

static int earlycon_configure(struct device_node *node, uint32 *clock)
{
	struct resource resource;
	uint32 shift = 0;
	uint32 width = 1;
	uint32 value;

	if (!node || !of_device_is_available(node) ||
	    !of_device_is_compatible(node, "ns16550a") ||
	    of_address_to_resource(node, 0, &resource) < 0)
		return -1;
	if (of_property_read_u32(node, "reg-shift", &value) == 0)
		shift = value;
	if (of_property_read_u32(node, "reg-io-width", &value) == 0)
		width = value;
	if (shift > 4 || (width != 1 && width != 4) ||
	    resource_size(&resource) < (UART_LSR << shift) + width)
		return -1;
	if (of_property_read_u32(node, "clock-frequency", &value) == 0 &&
	    value)
		*clock = value;
	early_console.address = resource.start;
	early_console.size = resource_size(&resource);
	early_console.shift = shift;
	early_console.width = width;
	return 0;
}

void earlycon_init(void)
{
	struct device_node *node = of_stdout_node();
	uint32 clock = EARLYCON_FALLBACK_CLOCK;
	uint32 divisor;

	early_console.address = EARLYCON_FALLBACK_ADDRESS;
	early_console.size = EARLYCON_FALLBACK_SIZE;
	early_console.shift = 0;
	early_console.width = 1;
	earlycon_configure(node, &clock);
	divisor = clock / (16 * 38400);
	if (!divisor)
		divisor = 1;
	if (divisor > 0xffff)
		divisor = 0xffff;
	earlycon_write(UART_IER, 0);
	earlycon_write(UART_LCR, UART_LCR_DLAB);
	earlycon_write(UART_RX, divisor & 0xff);
	earlycon_write(UART_IER, divisor >> 8);
	earlycon_write(UART_LCR, UART_LCR_8N1);
	earlycon_write(UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR);
}

void earlycon_putc(int character)
{
	while (!(earlycon_read(UART_LSR) & UART_LSR_THRE))
		;
	earlycon_write(UART_TX, character);
}

uint64 earlycon_address(void)
{
	return early_console.address;
}

uint64 earlycon_size(void)
{
	return early_console.size;
}
