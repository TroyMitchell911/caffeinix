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
	struct linux_termios termios;
	struct linux_winsize winsize;
	const struct tty_operations *operations;
	void *driver_data;
};

void tty_init(void);
int tty_register(struct tty *tty, const char *prefix, int line,
		 const struct tty_operations *operations, void *driver_data);
int tty_unregister(struct tty *tty);
void tty_receive_char(struct tty *tty, int character);
void tty_set_console(struct tty *tty);
struct tty *tty_get_console(void);
struct tty *tty_get(int line);

#endif
