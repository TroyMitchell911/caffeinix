#include <printf.h>
#include <stdarg.h>
#include <spinlock.h>
#include <debug.h>

static const char digits[] = "0123456789abcdef";
static struct {
        struct spinlock lock;
        /* This flag for panic */
        uint8 locking;
}pf;

static void console_emit(int character, void *context)
{
	(void)context;
	console_putc(character);
}

static void emit_number(printf_emit_t emit, void *context, uint64 number,
			uint8 base, int negative, int width, int zero_pad)
{
	char buffer[32];
	int length = 0;
	int padding;

        do {
		buffer[length++] = digits[number % base];
	} while ((number /= base) != 0);
	padding = width - length - negative;
	if (negative && zero_pad)
		emit('-', context);
	while (padding-- > 0)
		emit(zero_pad ? '0' : ' ', context);
	if (negative && !zero_pad)
		emit('-', context);
	while (--length >= 0)
		emit(buffer[length], context);
}

static void emit_pointer(printf_emit_t emit, void *context, uint64 pointer)
{
        int i;

	emit('0', context);
	emit('x', context);
	for (i = 0; i < 16; i++, pointer <<= 4)
		emit(digits[pointer >> (sizeof(uint64) * 8 - 4)], context);
}

void vprintf_emit(printf_emit_t emit, void *context, const char *fmt,
		  va_list arguments)
{
	int c, i;
	char *s;

	if (!emit || !fmt)
		PANIC("printf format");
	for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
		int is_long = 0;
		int width = 0;
		int zero_pad = 0;

		if (c != '%') {
			emit(c, context);
			continue;
		}
		c = fmt[++i] & 0xff;
		if (c == '0') {
			zero_pad = 1;
			c = fmt[++i] & 0xff;
		}
		while (c >= '0' && c <= '9') {
			width = width * 10 + c - '0';
			c = fmt[++i] & 0xff;
		}
		if (c == 'l') {
			is_long = 1;
			c = fmt[++i] & 0xff;
		}
		switch (c) {
		case 'd': {
			int64 value = is_long ? va_arg(arguments, int64) :
				va_arg(arguments, int);
			int negative = value < 0;
			uint64 magnitude = negative ? 0 - (uint64)value :
				(uint64)value;

			emit_number(emit, context, magnitude, 10, negative,
				    width, zero_pad);
			break;
		}
		case 'u':
			emit_number(emit, context,
				    is_long ? va_arg(arguments, uint64) :
				    va_arg(arguments, uint32),
				    10, 0, width, zero_pad);
			break;
		case 'x':
			emit_number(emit, context,
				    is_long ? va_arg(arguments, uint64) :
				    va_arg(arguments, uint32),
				    16, 0, width, zero_pad);
			break;
		case 'p':
			emit_pointer(emit, context, va_arg(arguments, uint64));
			break;
		case 's':
			s = va_arg(arguments, char *);
			if (!s)
				s = "(null)";
			while (*s)
				emit(*s++, context);
			break;
		case '%':
			emit('%', context);
			break;
		case 'c':
			emit(va_arg(arguments, int), context);
			break;
		default:
			emit('%', context);
			emit(c, context);
			break;
		}
        }
}

void printf(char* fmt, ...)
{
	va_list arguments;
	int locking = pf.locking;

	if (locking)
		spinlock_acquire(&pf.lock);
	va_start(arguments, fmt);
	vprintf_emit(console_emit, 0, fmt, arguments);
	va_end(arguments);

	if (locking)
		spinlock_release(&pf.lock);
}

void printf_enter_panic(void)
{
	pf.locking = 0;
}

void printf_init(void)
{
        spinlock_init(&pf.lock, "printf");
        /* No panic */
        pf.locking = 1;
}
