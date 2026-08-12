#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

static int test_metadata(void)
{
	struct termios original, changed, result;
	struct winsize winsize;
	struct stat statbuf;
	int fd, null_fd, tty_fd;

	fd = open("/dev/ttyS0", O_RDWR);
	if (fd < 0 || fstat(fd, &statbuf) || !S_ISCHR(statbuf.st_mode) ||
	    major(statbuf.st_rdev) != 4 || minor(statbuf.st_rdev) != 64)
		return fail("metadata", 10);
	errno = 0;
	tty_fd = open("/dev/tty", O_RDWR);
	if (tty_fd >= 0 || errno != ENXIO)
		return fail("controlling-tty", 11);
	null_fd = open("/dev/null", O_RDONLY);
	errno = 0;
	if (null_fd < 0 || isatty(null_fd) || errno != ENOTTY)
		return fail("notty", 12);
	if (close(null_fd))
		return fail("close-null", 13);
	if (tcgetattr(fd, &original))
		return fail("tcgets", 14);
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

static int test_raw(void)
{
	static const char expected[] = { 'R', '\r', 'X', '!' };
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
	if (!strcmp(argv[1], "raw"))
		return test_raw();
	if (!strcmp(argv[1], "long-output"))
		return test_long_output();
	return fail("mode", 2);
}
