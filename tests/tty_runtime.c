#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <unistd.h>

#define KERNEL_NCCS 19

static int fail(const char *test, int code)
{
	char message[96];
	int length;

	length = snprintf(message, sizeof(message),
	                  "TTY_RUNTIME_FAIL=%s:%d errno=%d\n",
	                  test, code, errno);
	write(1, message, length);
	return code;
}

static int write_all(int fd, const void *buffer, size_t count)
{
	const char *position = buffer;

	while (count) {
		ssize_t written = write(fd, position, count);

		if (written <= 0)
			return -1;
		position += written;
		count -= written;
	}
	return 0;
}

static volatile sig_atomic_t caught_signal;

static void catch_signal(int signal)
{
	caught_signal = signal;
}

static int expect_interrupted_read(int fd, int signal, const char *test,
				   int code)
{
	char buffer[16];

	errno = 0;
	if (read(fd, buffer, sizeof(buffer)) != -1 || errno != EINTR ||
	    caught_signal != signal)
		return fail(test, code);
	return 0;
}

static int test_metadata(void)
{
	struct termios original, changed, result;
	struct winsize winsize;
	struct stat statbuf;
	int fd, null_fd, pgid, sid, tty_fd;

	fd = open("/dev/ttyS0", O_RDWR);
	if (fd < 0 || fstat(fd, &statbuf) || !S_ISCHR(statbuf.st_mode) ||
	    major(statbuf.st_rdev) != 4 || minor(statbuf.st_rdev) != 64)
		return fail("metadata", 10);
	tty_fd = open("/dev/tty", O_RDWR);
	if (tty_fd < 0 || !isatty(tty_fd) ||
	    (pgid = tcgetpgrp(tty_fd)) != getpgrp() ||
	    ioctl(tty_fd, TIOCGSID, &sid) || sid != getsid(0) ||
	    tcsetpgrp(tty_fd, pgid))
		return fail("controlling-tty", 11);
	if (close(tty_fd))
		return fail("close-tty", 19);
	null_fd = open("/dev/null", O_RDONLY);
	errno = 0;
	if (null_fd < 0 || isatty(null_fd) || errno != ENOTTY)
		return fail("notty", 12);
	if (close(null_fd))
		return fail("close-null", 13);
	if (tcgetattr(fd, &original))
		return fail("tcgets", 14);
	if (!(original.c_lflag & ISIG) || original.c_cc[VINTR] != 3 ||
	    original.c_cc[VQUIT] != 28 || original.c_cc[VSUSP] != 26)
		return fail("signal-defaults", 20);
	changed = original;
	changed.c_lflag ^= ECHO;
	changed.c_cc[VMIN] = original.c_cc[VMIN] == 1 ? 2 : 1;
	if (tcsetattr(fd, TCSANOW, &changed) || tcgetattr(fd, &result) ||
	    result.c_iflag != changed.c_iflag ||
	    result.c_oflag != changed.c_oflag ||
	    result.c_cflag != changed.c_cflag ||
	    result.c_lflag != changed.c_lflag ||
	    memcmp(result.c_cc, changed.c_cc, KERNEL_NCCS))
		return fail("termios-roundtrip", 15);
	if (tcsetattr(fd, TCSANOW, &original))
		return fail("termios-restore", 16);
	memset(&winsize, 0, sizeof(winsize));
	if (ioctl(fd, TIOCGWINSZ, &winsize) ||
	    winsize.ws_row != 24 || winsize.ws_col != 80)
		return fail("winsize", 17);
	if (close(fd))
		return fail("close", 18);
	write_all(1, "TTY_METADATA_OK\n", 16);
	return 0;
}

static int test_canonical(void)
{
	static const char expected[] = "abd\n";
	struct termios original, mode;
	char buffer[16];
	ssize_t count;
	int fd;

	fd = open("/dev/ttyS0", O_RDWR);
	if (fd < 0 || tcgetattr(fd, &original))
		return fail("canonical-open", 20);
	mode = original;
	mode.c_iflag |= ICRNL;
	mode.c_lflag |= ICANON | ECHO | ECHOE;
	mode.c_cc[VERASE] = 0x7f;
	if (tcsetattr(fd, TCSANOW, &mode))
		return fail("canonical-set", 21);
	write_all(1, "TTY_CANONICAL_READY\n", 20);
	count = read(fd, buffer, sizeof(buffer));
	if (tcsetattr(fd, TCSANOW, &original))
		return fail("canonical-restore", 22);
	if (count != sizeof(expected) - 1 ||
	    memcmp(buffer, expected, sizeof(expected) - 1))
		return fail("canonical-data", 23);
	write_all(1, "TTY_CANONICAL_OK\n", 17);
	close(fd);
	return 0;
}

static int test_nonblock(void)
{
	char value;
	int fd, flags;

	fd = open("/dev/ttyS0", O_RDONLY);
	if (fd < 0)
		return fail("nonblock-open", 50);
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return fail("nonblock-flags", 51);
	errno = 0;
	if (read(fd, &value, 1) != -1 || errno != EAGAIN)
		return fail("nonblock-read", 52);
	if (close(fd))
		return fail("nonblock-close", 53);
	write_all(1, "TTY_NONBLOCK_OK\n", 16);
	return 0;
}

static int test_raw(void)
{
	static const char expected[] = { 'R', '\r', 0x1d, 'X', '!' };
	struct termios original, mode;
	char buffer[sizeof(expected)];
	size_t total = 0;
	int fd;

	fd = open("/dev/ttyS0", O_RDWR);
	if (fd < 0 || tcgetattr(fd, &original))
		return fail("raw-open", 30);
	mode = original;
	mode.c_iflag &= ~ICRNL;
	mode.c_lflag &= ~(ICANON | ECHO);
	mode.c_cc[VMIN] = 1;
	if (tcsetattr(fd, TCSANOW, &mode))
		return fail("raw-set", 31);
	write_all(1, "TTY_RAW_READY\n", 14);
	while (total < sizeof(buffer)) {
		ssize_t count = read(fd, buffer + total, sizeof(buffer) - total);

		if (count <= 0)
			break;
		total += count;
	}
	if (tcsetattr(fd, TCSANOW, &original))
		return fail("raw-restore", 32);
	if (total != sizeof(expected) || memcmp(buffer, expected,
	                                      sizeof(expected)))
		return fail("raw-data", 33);
	write_all(1, "TTY_RAW_OK\n", 11);
	close(fd);
	return 0;
}

static int test_signals(void)
{
	static const char preserved[] = "kept\n";
	struct sigaction action;
	struct termios original, mode;
	char buffer[16];
	ssize_t count;
	int fd, flags;

	fd = open("/dev/ttyS0", O_RDWR);
	if (fd < 0 || tcgetattr(fd, &original))
		return fail("signals-open", 60);
	memset(&action, 0, sizeof(action));
	action.sa_handler = catch_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGQUIT, &action, NULL) ||
	    sigaction(SIGTSTP, &action, NULL))
		return fail("signals-handler", 61);
	mode = original;
	mode.c_iflag |= ICRNL;
	mode.c_lflag |= ICANON | ISIG;
	mode.c_lflag &= ~(ECHO | NOFLSH);
	mode.c_cc[VINTR] = 3;
	mode.c_cc[VQUIT] = 28;
	mode.c_cc[VSUSP] = 26;
	if (tcsetattr(fd, TCSANOW, &mode))
		return fail("signals-mode", 62);

	caught_signal = 0;
	write_all(1, "TTY_SIGNAL_FLUSH_READY\n",
	          sizeof("TTY_SIGNAL_FLUSH_READY\n") - 1);
	if (expect_interrupted_read(fd, SIGINT, "signal-int", 63))
		return 63;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return fail("signal-flush-flags", 64);
	errno = 0;
	if (read(fd, buffer, sizeof(buffer)) != -1 || errno != EAGAIN)
		return fail("signal-flush-data", 65);
	if (fcntl(fd, F_SETFL, flags) < 0)
		return fail("signal-flush-restore", 66);
	write_all(1, "TTY_SIGNAL_FLUSH_OK\n",
	          sizeof("TTY_SIGNAL_FLUSH_OK\n") - 1);

	mode.c_lflag |= NOFLSH;
	if (tcsetattr(fd, TCSANOW, &mode))
		return fail("noflsh-mode", 67);
	caught_signal = 0;
	write_all(1, "TTY_NOFLSH_READY\n",
	          sizeof("TTY_NOFLSH_READY\n") - 1);
	if (expect_interrupted_read(fd, SIGINT, "noflsh-int", 68))
		return 68;
	write_all(1, "TTY_NOFLSH_LINE_READY\n",
	          sizeof("TTY_NOFLSH_LINE_READY\n") - 1);
	count = read(fd, buffer, sizeof(buffer));
	if (count != sizeof(preserved) - 1 ||
	    memcmp(buffer, preserved, sizeof(preserved) - 1))
		return fail("noflsh-data", 69);
	write_all(1, "TTY_NOFLSH_OK\n", sizeof("TTY_NOFLSH_OK\n") - 1);

	mode.c_lflag &= ~NOFLSH;
	if (tcsetattr(fd, TCSANOW, &mode))
		return fail("quit-mode", 70);
	caught_signal = 0;
	write_all(1, "TTY_QUIT_READY\n", sizeof("TTY_QUIT_READY\n") - 1);
	if (expect_interrupted_read(fd, SIGQUIT, "signal-quit", 71))
		return 71;
	write_all(1, "TTY_QUIT_OK\n", sizeof("TTY_QUIT_OK\n") - 1);
	caught_signal = 0;
	write_all(1, "TTY_SUSPEND_READY\n",
	          sizeof("TTY_SUSPEND_READY\n") - 1);
	if (expect_interrupted_read(fd, SIGTSTP, "signal-suspend", 72))
		return 72;
	write_all(1, "TTY_SUSPEND_OK\n", sizeof("TTY_SUSPEND_OK\n") - 1);

	if (tcsetattr(fd, TCSANOW, &original) || close(fd))
		return fail("signals-restore", 73);
	return 0;
}

static int test_long_output(void)
{
	char output[1024];
	int fd;

	memset(output, 'U', sizeof(output));
	fd = open("/dev/ttyS0", O_WRONLY);
	if (fd < 0)
		return fail("long-open", 40);
	write_all(1, "TTY_LONG_BEGIN\n", 15);
	if (write_all(fd, output, sizeof(output)) ||
	    write_all(fd, "\n", 1))
		return fail("long-write", 41);
	write_all(1, "TTY_LONG_END\n", 13);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return fail("arguments", 1);
	if (!strcmp(argv[1], "metadata"))
		return test_metadata();
	if (!strcmp(argv[1], "canonical"))
		return test_canonical();
	if (!strcmp(argv[1], "nonblock"))
		return test_nonblock();
	if (!strcmp(argv[1], "raw"))
		return test_raw();
	if (!strcmp(argv[1], "signals"))
		return test_signals();
	if (!strcmp(argv[1], "long-output"))
		return test_long_output();
	return fail("mode", 2);
}
