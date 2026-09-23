/*
 * Freestanding formatting and serialized polling console output.
 *
 * The formatter supports the subset needed by the kernel, not the complete
 * libc printf ABI. Callback emission separates text formatting from storage
 * or UART output; panic output deliberately bypasses normal locking.
 */
#include <printf.h>
#include <stdarg.h>
#include <spinlock.h>
#include <debug.h>

static const char digits[] = "0123456789abcdef";
static struct {
        struct spinlock lock;
	uint8 locking;
}pf;

/*
 * Formatter callback for the polling console; context is unused because
 * console selection is global.
 */
static void console_emit(int character, void *context)
{
	(void)context;
	console_putc(character);
}

/*
 * Generate digits in reverse in a fixed local buffer, then emit sign and
 * padding in the requested order. Callers constrain base to 10 or 16.
 */
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

/*
 * Print the full RV64 address as a fixed-width hexadecimal value so leading
 * zeroes are retained in diagnostics.
 */
static void emit_pointer(printf_emit_t emit, void *context, uint64 pointer)
{
        int i;

	emit('0', context);
	emit('x', context);
	for (i = 0; i < 16; i++, pointer <<= 4)
		emit(digits[pointer >> (sizeof(uint64) * 8 - 4)], context);
}

/**
 * vprintf_emit() - Format text through a caller-provided sink
 * @emit: Non-NULL callback consuming one character synchronously.
 * @context: Borrowed opaque pointer passed unchanged to @emit.
 * @fmt: Non-NULL format string; must remain readable throughout the call.
 * @arguments: Variadic argument list matching the supported conversions.
 *
 * Supports d, u, x, p, s, c and percent, with decimal width, zero padding and
 * a single l integer modifier. It does not implement the full libc format
 * language. The callback may not retain borrowed stack state.
 *
 * Context: Determined by @emit; formatter itself takes no locks and does not
 *          sleep.
 */
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

/**
 * printf() - Print serialized kernel text without a timestamp
 * @fmt: Format string accepted by vprintf_emit().
 * @...: Arguments matching @fmt.
 *
 * Context: Non-sleeping console context; holds the printf spinlock unless
 *          panic mode disabled locking. Must not recursively call printf().
 */
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

/**
 * printf_emergency() - Print diagnostics without the normal output lock
 * @fmt: Format string accepted by vprintf_emit().
 * @...: Arguments matching @fmt.
 *
 * Bypassing serialization avoids depending on a lock held by a faulting
 * context; this is not a general replacement for printf().
 *
 * Context: Non-sleeping diagnostic context; may interleave with other output.
 */
void printf_emergency(char *fmt, ...)
{
	va_list arguments;

	va_start(arguments, fmt);
	vprintf_emit(console_emit, 0, fmt, arguments);
	va_end(arguments);
}

/**
 * printf_enter_panic() - Disable ordinary output locking permanently
 *
 * Context: Fatal-error path only; no return to normal concurrent operation.
 */
void printf_enter_panic(void)
{
	pf.locking = 0;
}

/**
 * printf_init() - Enable serialized console formatting
 *
 * Context: Boot once before concurrent printf() callers.
 */
void printf_init(void)
{
        spinlock_init(&pf.lock, "printf");
	pf.locking = 1;
}
