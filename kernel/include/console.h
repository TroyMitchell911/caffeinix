#ifndef __CAFFEINIX_KERNEL_CONSOLE_H
#define __CAFFEINIX_KERNEL_CONSOLE_H

#include <typedefs.h>

struct device_node;
struct tty;
struct console;

struct console_operations {
	void (*put_char)(struct console *console, int character);
};

struct console {
	const char *name;
	const struct console_operations *operations;
	struct device_node *of_node;
	struct tty *tty;
	void *private;
	int registered;
};

void console_early_init(void);
int console_register(struct console *console);
void console_unregister(struct console *console);
void console_putc(int character);

#endif
