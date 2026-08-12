#include <console.h>
#include <device_model.h>
#include <earlycon.h>
#include <of.h>
#include <spinlock.h>
#include <tty.h>

#define CONSOLE_MAX 8

static struct {
	struct spinlock lock;
	struct console *devices[CONSOLE_MAX];
	struct console *active;
} console_core;

void console_early_init(void)
{
	spinlock_init(&console_core.lock, "console core");
	earlycon_init();
}

static int console_is_stdout(struct console *console)
{
	struct device_node *stdout_node = of_stdout_node();

	return stdout_node && console->of_node == stdout_node;
}

int console_register(struct console *console)
{
	int free_slot = -1;
	int index;

	if (!console || !console->name || !console->operations ||
	    !console->operations->put_char || !console->tty)
		return DRIVER_ERR_INVAL;
	spinlock_acquire(&console_core.lock);
	if (console->registered) {
		spinlock_release(&console_core.lock);
		return DRIVER_ERR_EXIST;
	}
	for (index = 0; index < CONSOLE_MAX; index++) {
		if (!console_core.devices[index] && free_slot < 0)
			free_slot = index;
		if (console_core.devices[index] == console) {
			spinlock_release(&console_core.lock);
			return DRIVER_ERR_EXIST;
		}
	}
	if (free_slot < 0) {
		spinlock_release(&console_core.lock);
		return DRIVER_ERR_BUSY;
	}
	console_core.devices[free_slot] = console;
	console->registered = 1;
	if (!console_core.active || console_is_stdout(console))
		console_core.active = console;
	console = console_core.active;
	spinlock_release(&console_core.lock);
	tty_set_console(console->tty);
	return DRIVER_OK;
}

void console_unregister(struct console *console)
{
	struct console *replacement = 0;
	int index;

	if (!console)
		return;
	spinlock_acquire(&console_core.lock);
	for (index = 0; index < CONSOLE_MAX; index++) {
		if (console_core.devices[index] == console)
			console_core.devices[index] = 0;
		if (!replacement && console_core.devices[index])
			replacement = console_core.devices[index];
		if (console_core.devices[index] &&
		    console_is_stdout(console_core.devices[index]))
			replacement = console_core.devices[index];
	}
	if (console_core.active == console)
		console_core.active = replacement;
	console->registered = 0;
	replacement = console_core.active;
	spinlock_release(&console_core.lock);
	if (replacement)
		tty_set_console(replacement->tty);
}

void console_putc(int character)
{
	struct console *console = console_core.active;

	if (console && console->registered)
		console->operations->put_char(console, character);
	else
		earlycon_putc(character);
}
