/*
 * Kernel console selection and early-console fallback.
 *
 * The registry selects the Device Tree stdout device when available and
 * exposes its TTY as /dev/console. Character output is polling and
 * intentionally separate from userspace TTY buffering. Registration and
 * object destruction require boot/lifecycle serialization; the output fast
 * path takes no device reference.
 */
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

/**
 * console_early_init() - Prepare registry locking and early output
 *
 * Context: Boot hart before console registration; does not sleep.
 */
void console_early_init(void)
{
	spinlock_init(&console_core.lock, "console core");
	earlycon_init();
}

/*
 * Match the console against the already-discovered Device Tree stdout node;
 * the node identity remains stable throughout console registration.
 */
static int console_is_stdout(struct console *console)
{
	struct device_node *stdout_node = of_stdout_node();

	return stdout_node && console->of_node == stdout_node;
}

/**
 * console_register() - Publish a driver-owned console
 * @console: Live object with name, polling put_char operation, and TTY.
 *
 * The object is not copied or reference-counted. The caller keeps it alive
 * until output can no longer access it. The first console is selected unless
 * a stdout-path match replaces it.
 *
 * Context: Serialized driver registration; registry updates take a spinlock.
 * Return: %DRIVER_OK, %DRIVER_ERR_INVAL, %DRIVER_ERR_EXIST, or
 *         %DRIVER_ERR_BUSY when all registry slots are used.
 */
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

/**
 * console_unregister() - Remove a console and select a replacement
 * @console: Driver-owned object to remove; NULL is ignored.
 *
 * Registry removal does not wait for polling writers already using the old
 * object. If another console remains, its TTY becomes the selected console.
 *
 * Context: Lifecycle-serialized removal; caller must exclude concurrent
 *          output before freeing the object.
 */
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

/**
 * console_putc() - Emit one character through the selected console
 * @character: Character value passed to the polling backend.
 *
 * No allocation, scheduler sleep, or registry lock is used. With no
 * registered active console, the early-console backend is used.
 *
 * Context: Boot, thread, or interrupt context; busy-waits in the backend and
 *          must not recurse through console output.
 */
void console_putc(int character)
{
	struct console *console = console_core.active;

	if (console && console->registered)
		console->operations->put_char(console, character);
	else
		earlycon_putc(character);
}
