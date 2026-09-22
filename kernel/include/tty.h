/* TTY line-discipline and console-selection interface. */
#ifndef __CAFFEINIX_KERNEL_TTY_H
#define __CAFFEINIX_KERNEL_TTY_H

#include <char_device.h>
#include <linux_uapi.h>
#include <spinlock.h>
#include <typedefs.h>
#include <wait.h>

#define TTY_MAX_DEVICES 8
#define TTY_INPUT_SIZE 256

struct tty;

struct tty_operations {
	int64 (*write)(struct tty *tty, const char *buffer, uint64 count);
	void (*put_char)(struct tty *tty, int character);
};

struct tty {
	struct spinlock lock;
	struct wait_queue read_wait;
	int registered;
	int line;
	char name[CHAR_DEVICE_NAME_MAX + 1];
	char input[TTY_INPUT_SIZE];
	uint64 read_position;
	uint64 commit_position;
	uint64 edit_position;
	int eof_pending;
	/* Process job-control state is protected by the process table lock. */
	int session_id;
	int foreground_pgid;
	struct linux_termios termios;
	struct linux_winsize winsize;
	const struct tty_operations *operations;
	void *driver_data;
};

/**
 * tty_init() - Initialize the TTY registry.
 *
 * Context:
 * Early boot before UART registration; does not sleep.
 */
void tty_init(void);
/**
 * tty_register() - Register a hardware-backed TTY line.
 * @tty: Caller-owned TTY storage.
 * @prefix: Stable device-name prefix such as "ttyS".
 * @line: Requested line below TTY_MAX_DEVICES, or any negative value to
 * select the lowest free line.
 * @operations: Non-NULL backend operations retained by @tty.
 * @driver_data: Backend-private pointer retained by @tty.
 *
 * Context:
 * Process context; creates a character device.
 * Return:
 * VFS_OK on success, or a negative VFS error for invalid arguments,
 * unavailable lines, name construction, or character-device registration.
 */
int tty_register(struct tty *tty, const char *prefix, int line,
		 const struct tty_operations *operations, void *driver_data);
/**
 * tty_unregister() - Remove a TTY line and its character device.
 * @tty: Registered TTY.
 *
 * Context:
 * Process context; callers must stop hardware callbacks first.
 * Return:
 * VFS_OK on success, or a negative VFS error for an absent or busy TTY or
 * failure to unregister its character-device node.
 */
int tty_unregister(struct tty *tty);
/**
 * tty_receive_char() - Feed one received character into a TTY.
 * @tty: Registered TTY.
 * @character: Received byte or %UART_RX_BREAK marker.
 *
 * Context:
 * IRQ-safe; serializes input and termios under the TTY lock.
 */
void tty_receive_char(struct tty *tty, int character);
/**
 * tty_set_console() - Select a registered TTY as the system console.
 * @tty: Registered TTY; NULL or an unregistered TTY leaves selection unchanged.
 *
 * Context:
 * Process context during serial setup; excludes concurrent unregister.
 * Takes the registry spinlock to change the selection; does not sleep.
 */
void tty_set_console(struct tty *tty);
/**
 * tty_get_console() - Return the currently selected console TTY.
 *
 * Context:
 * Any context; caller must not retain it across unregister.
 * Return:
 * Selected TTY or %NULL.
 */
struct tty *tty_get_console(void);
/**
 * tty_get() - Look up a TTY by line number.
 * @line: Line in the range zero through %TTY_MAX_DEVICES - 1.
 *
 * The pointer is borrowed; this does not pin the TTY. The caller must prevent
 * concurrent unregister while using it.
 *
 * Context:
 * Any context; does not sleep.
 * Return:
 * TTY or %NULL when absent.
 */
struct tty *tty_get(int line);
/**
 * tty_device_number() - Return the Linux-compatible device number for a TTY.
 * @tty: Registered TTY.
 *
 * Context:
 * Any context while @tty remains registered.
 * Return:
 * Encoded major/minor number, or zero for %NULL.
 */
uint32 tty_device_number(const struct tty *tty);

#endif
