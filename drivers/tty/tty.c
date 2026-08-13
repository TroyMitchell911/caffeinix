#include <char_device.h>
#include <debug.h>
#include <device.h>
#include <mystring.h>
#include <process.h>
#include <spinlock.h>
#include <tty.h>

#define TTY_SERIAL_MAJOR 4
#define TTY_SERIAL_MINOR 64

static struct {
	struct spinlock lock;
	struct tty *devices[TTY_MAX_DEVICES];
	struct tty *console;
	struct char_device serial_device;
	struct char_device tty_device;
	struct char_device console_device;
} tty_core;

static void tty_default_termios(struct tty *tty)
{
	memset(&tty->termios, 0, sizeof(tty->termios));
	tty->termios.iflag = LINUX_ICRNL;
	tty->termios.cflag = LINUX_B38400 | LINUX_CS8 |
	                     LINUX_CREAD | LINUX_CLOCAL;
	tty->termios.lflag = LINUX_ICANON | LINUX_ECHO |
	                     LINUX_ECHOE | LINUX_ECHOK;
	tty->termios.control[LINUX_VINTR] = 3;
	tty->termios.control[LINUX_VERASE] = 0x7f;
	tty->termios.control[LINUX_VEOF] = 4;
	tty->termios.control[LINUX_VMIN] = 1;
	memset(&tty->winsize, 0, sizeof(tty->winsize));
	tty->winsize.rows = 24;
	tty->winsize.columns = 80;
}

static struct tty *tty_from_device(struct char_device *device,
				   struct vfs_file *file)
{
	uint64 number = file->path.dentry->inode->device;
	uint32 minor = VFS_DEVICE_MINOR(number);

	if (device == &tty_core.console_device)
		return tty_get_console();
	if (device != &tty_core.serial_device ||
	    VFS_DEVICE_MAJOR(number) != TTY_SERIAL_MAJOR ||
	    minor < TTY_SERIAL_MINOR ||
	    minor >= TTY_SERIAL_MINOR + TTY_MAX_DEVICES)
		return 0;
	return tty_get(minor - TTY_SERIAL_MINOR);
}

static int tty_device_open(struct char_device *device,
			   struct vfs_file *file)
{
	if (device == &tty_core.tty_device)
		return VFS_ERR_NXIO;
	return tty_from_device(device, file) ? VFS_OK : VFS_ERR_NODEV;
}

static int64 tty_read(struct tty *tty, int user_destination,
		      uint64 destination, uint64 count)
{
	uint64 total = 0;
	int canonical;

	if (!count)
		return 0;
	spinlock_acquire(&tty->lock);
	canonical = tty->termios.lflag & LINUX_ICANON;
	while (total < count) {
		while (tty->read_position == tty->commit_position &&
		       !tty->eof_pending) {
			if (!canonical && !tty->termios.control[LINUX_VMIN]) {
				spinlock_release(&tty->lock);
				return total;
			}
			wait_queue_sleep(&tty->read_wait, &tty->lock);
		}
		if (tty->read_position == tty->commit_position &&
		    tty->eof_pending) {
			tty->eof_pending = 0;
			break;
		}
		while (total < count &&
		       tty->read_position != tty->commit_position) {
			char character =
				tty->input[tty->read_position % TTY_INPUT_SIZE];

			if (either_copyout(user_destination, destination + total,
			                   &character, 1) < 0) {
				spinlock_release(&tty->lock);
				return total ? total : VFS_ERR_FAULT;
			}
			tty->read_position++;
			total++;
			if (canonical && character == '\n')
				break;
		}
		if (canonical || total)
			break;
	}
	if (tty->eof_pending &&
	    tty->read_position == tty->commit_position)
		tty->eof_pending = 0;
	spinlock_release(&tty->lock);
	return total;
}

static int64 tty_write(struct tty *tty, int user_source, uint64 source,
		       uint64 count)
{
	char buffer[64];
	uint64 total = 0;

	if (!tty->operations || !tty->operations->write)
		return VFS_ERR_NODEV;
	while (total < count) {
		uint64 remaining = count - total;
		uint64 chunk = remaining > sizeof(buffer) ?
			       sizeof(buffer) : remaining;
		int64 written;

		if (either_copyin(buffer, user_source, source + total, chunk) < 0)
			return total ? total : VFS_ERR_FAULT;
		written = tty->operations->write(tty, buffer, chunk);
		if (written < 0)
			return total ? total : written;
		if (!written)
			break;
		total += written;
	}
	return total;
}

static int64 tty_device_read(struct char_device *device,
			     struct vfs_file *file, int user_destination,
			     uint64 destination, uint64 count)
{
	struct tty *tty = tty_from_device(device, file);

	return tty ? tty_read(tty, user_destination, destination, count) :
		     VFS_ERR_NODEV;
}

static int64 tty_device_write(struct char_device *device,
			      struct vfs_file *file, int user_source,
			      uint64 source, uint64 count)
{
	struct tty *tty = tty_from_device(device, file);

	return tty ? tty_write(tty, user_source, source, count) :
		     VFS_ERR_NODEV;
}

static int64 tty_device_ioctl(struct char_device *device,
			      struct vfs_file *file, uint64 request,
			      uint64 argument)
{
	struct linux_termios termios;
	struct linux_winsize winsize;
	struct tty *tty = tty_from_device(device, file);

	if (!tty)
		return VFS_ERR_NODEV;
	if (request == LINUX_TCGETS) {
		spinlock_acquire(&tty->lock);
		termios = tty->termios;
		spinlock_release(&tty->lock);
		return either_copyout(1, argument, &termios,
		                      sizeof(termios)) < 0 ?
			VFS_ERR_FAULT : VFS_OK;
	}
	if (request == LINUX_TCSETS || request == LINUX_TCSETSW ||
	    request == LINUX_TCSETSF) {
		if (either_copyin(&termios, 1, argument, sizeof(termios)) < 0)
			return VFS_ERR_FAULT;
		spinlock_acquire(&tty->lock);
		tty->termios = termios;
		if (request == LINUX_TCSETSF) {
			tty->read_position = tty->edit_position;
			tty->commit_position = tty->edit_position;
			tty->eof_pending = 0;
		}
		spinlock_release(&tty->lock);
		return VFS_OK;
	}
	if (request == LINUX_TIOCGWINSZ) {
		spinlock_acquire(&tty->lock);
		winsize = tty->winsize;
		spinlock_release(&tty->lock);
		return either_copyout(1, argument, &winsize,
		                      sizeof(winsize)) < 0 ?
			VFS_ERR_FAULT : VFS_OK;
	}
	return VFS_ERR_NOTTY;
}

static uint32 tty_device_poll(struct char_device *device,
			      struct vfs_file *file, uint32 events)
{
	struct tty *tty = tty_from_device(device, file);
	uint32 ready = 0;

	if (!tty)
		return VFS_POLL_ERR;
	spinlock_acquire(&tty->lock);
	if ((events & VFS_POLL_IN) &&
	    (tty->read_position != tty->commit_position ||
	     tty->eof_pending))
		ready |= VFS_POLL_IN;
	if ((events & VFS_POLL_OUT) && tty->operations &&
	    tty->operations->write)
		ready |= VFS_POLL_OUT;
	spinlock_release(&tty->lock);
	return ready;
}

static const struct char_device_operations tty_operations = {
	.open = tty_device_open,
	.read = tty_device_read,
	.write = tty_device_write,
	.ioctl = tty_device_ioctl,
	.poll = tty_device_poll,
};

static void tty_echo(struct tty *tty, int character)
{
	if (!tty->operations || !tty->operations->put_char)
		return;
	if (character == 0x100) {
		tty->operations->put_char(tty, '\b');
		tty->operations->put_char(tty, ' ');
		tty->operations->put_char(tty, '\b');
		return;
	}
	tty->operations->put_char(tty, character);
}

void tty_receive_char(struct tty *tty, int character)
{
	int canonical, echo, notify = 0;

	if (!tty || !tty->registered)
		return;
	spinlock_acquire(&tty->lock);
	canonical = tty->termios.lflag & LINUX_ICANON;
	echo = tty->termios.lflag & LINUX_ECHO;
	if (character == '\r' && (tty->termios.iflag & LINUX_ICRNL))
		character = '\n';
	if (canonical &&
	    character == tty->termios.control[LINUX_VERASE]) {
		if (tty->edit_position != tty->commit_position) {
			tty->edit_position--;
			if (tty->termios.lflag & LINUX_ECHOE)
				tty_echo(tty, 0x100);
		}
		spinlock_release(&tty->lock);
		return;
	}
	if (canonical && character == tty->termios.control[LINUX_VEOF]) {
		tty->commit_position = tty->edit_position;
		tty->eof_pending = 1;
		wait_queue_wake_all(&tty->read_wait);
		spinlock_release(&tty->lock);
		vfs_poll_notify();
		return;
	}
	if (tty->edit_position - tty->read_position >= TTY_INPUT_SIZE) {
		spinlock_release(&tty->lock);
		return;
	}
	tty->input[tty->edit_position++ % TTY_INPUT_SIZE] = character;
	if (echo)
		tty_echo(tty, character);
	if (!canonical || character == '\n' ||
	    tty->edit_position - tty->read_position == TTY_INPUT_SIZE) {
		tty->commit_position = tty->edit_position;
		wait_queue_wake_all(&tty->read_wait);
		notify = 1;
	}
	spinlock_release(&tty->lock);
	if (notify)
		vfs_poll_notify();
}

static int make_tty_name(char *name, uint32 size, const char *prefix,
			 int line)
{
	uint32 length = strlen(prefix);

	if (line < 0 || line > 9 || length + 2 > size)
		return VFS_ERR_INVAL;
	memmove(name, prefix, length);
	name[length] = '0' + line;
	name[length + 1] = 0;
	return VFS_OK;
}

int tty_register(struct tty *tty, const char *prefix, int line,
		 const struct tty_operations *operations, void *driver_data)
{
	uint64 device;
	int selected = line;
	int status;

	if (!tty || !prefix || !operations || !operations->write ||
	    !operations->put_char)
		return VFS_ERR_INVAL;
	spinlock_acquire(&tty_core.lock);
	if (selected < 0) {
		for (selected = 0; selected < TTY_MAX_DEVICES; selected++)
			if (!tty_core.devices[selected])
				break;
	}
	if (selected < 0 || selected >= TTY_MAX_DEVICES ||
	    tty_core.devices[selected] || tty->registered) {
		spinlock_release(&tty_core.lock);
		return VFS_ERR_BUSY;
	}
	status = make_tty_name(tty->name, sizeof(tty->name), prefix,
	                       selected);
	if (status < 0) {
		spinlock_release(&tty_core.lock);
		return status;
	}
	tty->line = selected;
	tty->operations = operations;
	tty->driver_data = driver_data;
	tty_default_termios(tty);
	spinlock_init(&tty->lock, tty->name);
	wait_queue_init(&tty->read_wait, tty->name);
	tty->registered = 1;
	tty_core.devices[selected] = tty;
	if (!tty_core.console)
		tty_core.console = tty;
	spinlock_release(&tty_core.lock);
	device = VFS_MAKE_DEVICE(TTY_SERIAL_MAJOR,
	                         TTY_SERIAL_MINOR + selected);
	status = char_device_node_register(tty->name, device, 0600);
	if (status < 0) {
		spinlock_acquire(&tty_core.lock);
		if (tty_core.console == tty)
			tty_core.console = 0;
		tty_core.devices[selected] = 0;
		tty->registered = 0;
		spinlock_release(&tty_core.lock);
	}
	return status;
}

int tty_unregister(struct tty *tty)
{
	int status;

	if (!tty || !tty->registered)
		return VFS_ERR_NOENT;
	spinlock_acquire(&tty->lock);
	if (!wait_queue_empty(&tty->read_wait)) {
		spinlock_release(&tty->lock);
		return VFS_ERR_BUSY;
	}
	spinlock_release(&tty->lock);
	status = char_device_node_unregister(tty->name);
	if (status < 0)
		return status;
	spinlock_acquire(&tty_core.lock);
	if (tty_core.console == tty)
		tty_core.console = 0;
	tty_core.devices[tty->line] = 0;
	tty->registered = 0;
	spinlock_release(&tty_core.lock);
	return VFS_OK;
}

void tty_set_console(struct tty *tty)
{
	if (!tty || !tty->registered)
		return;
	spinlock_acquire(&tty_core.lock);
	tty_core.console = tty;
	spinlock_release(&tty_core.lock);
}

struct tty *tty_get_console(void)
{
	struct tty *tty;

	spinlock_acquire(&tty_core.lock);
	tty = tty_core.console;
	spinlock_release(&tty_core.lock);
	return tty;
}

struct tty *tty_get(int line)
{
	struct tty *tty = 0;

	if (line < 0 || line >= TTY_MAX_DEVICES)
		return 0;
	spinlock_acquire(&tty_core.lock);
	tty = tty_core.devices[line];
	spinlock_release(&tty_core.lock);
	return tty;
}

void tty_init(void)
{
	uint64 serial = VFS_MAKE_DEVICE(TTY_SERIAL_MAJOR, TTY_SERIAL_MINOR);

	spinlock_init(&tty_core.lock, "tty core");
	tty_core.serial_device.flags = CHAR_DEVICE_TERMINAL;
	tty_core.serial_device.operations = &tty_operations;
	tty_core.tty_device.flags = CHAR_DEVICE_TERMINAL;
	tty_core.tty_device.operations = &tty_operations;
	tty_core.console_device.flags = CHAR_DEVICE_TERMINAL;
	tty_core.console_device.operations = &tty_operations;
	if (char_device_region_register(serial, TTY_MAX_DEVICES,
	                                "serial") < 0 ||
	    char_device_add(&tty_core.serial_device, serial,
	                    TTY_MAX_DEVICES) < 0 ||
	    char_device_region_register(DEVICE_TTY, 2, "tty") < 0 ||
	    char_device_add(&tty_core.tty_device, DEVICE_TTY, 1) < 0 ||
	    char_device_add(&tty_core.console_device, DEVICE_CONSOLE, 1) < 0 ||
	    char_device_node_register("tty", DEVICE_TTY, 0666) < 0 ||
	    char_device_node_register("console", DEVICE_CONSOLE, 0600) < 0)
		PANIC("register TTY character devices");
}
