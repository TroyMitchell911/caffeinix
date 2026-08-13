#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIXTURE_ADDRESS "10.0.2.2"
#define TCP_BULK_PORT 18083
#define CPU_WORKERS 8
#define IO_WORKERS 6
#define NET_WORKERS 4
#define WORKERS (CPU_WORKERS + IO_WORKERS + NET_WORKERS)
#define CPU_ITERATIONS 6000000
#define BLOCK_SIZE 4096
#define IO_BLOCKS 24
#define IO_ROUNDS 3
#define NET_BLOCKS 8
#define NET_ROUNDS 6

static unsigned char buffer[BLOCK_SIZE];

static int fail(const char *operation, int code)
{
	printf("PRESSURE_RUNTIME_FAIL %s:%d errno=%d\n",
	       operation, code, errno);
	return 1;
}

static int write_all(int fd, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t offset = 0;

	while (offset < length) {
		ssize_t written = write(fd, bytes + offset, length - offset);

		if (written <= 0)
			return -1;
		offset += written;
	}
	return 0;
}

static int read_all(int fd, void *data, size_t length)
{
	unsigned char *bytes = data;
	size_t offset = 0;

	while (offset < length) {
		ssize_t received = read(fd, bytes + offset, length - offset);

		if (received <= 0)
			return -1;
		offset += received;
	}
	return 0;
}

static void cpu_worker(int index)
{
	volatile uint64_t value = index + 1;
	int iteration;

	if ((index & 1) && setpriority(PRIO_PROCESS, 0, 5) < 0)
		_exit(10);
	for (iteration = 0; iteration < CPU_ITERATIONS; iteration++)
		value = value * 6364136223846793005ULL + 1;
	_exit(value == UINT64_MAX ? 11 : 0);
}

static void fill_block(int worker, int round, int block)
{
	int index;

	for (index = 0; index < BLOCK_SIZE; index++)
		buffer[index] = worker * 29 + round * 17 + block * 7 + index;
}

static int verify_block(int worker, int round, int block)
{
	int index;

	for (index = 0; index < BLOCK_SIZE; index++) {
		unsigned char expected;

		expected = worker * 29 + round * 17 + block * 7 + index;
		if (buffer[index] != expected)
			return -1;
	}
	return 0;
}

static void io_worker(int index)
{
	static const char *directories[] = {
		"/pressure-ext", "/pressure-ext",
		"/tmp/pressure-tmp", "/tmp/pressure-tmp",
		"/mnt/fat/pressure-fat", "/mnt/fat/pressure-fat",
	};
	char path[64];
	int block, fd, round;

	snprintf(path, sizeof(path), "%s-%d", directories[index], index);
	for (round = 0; round < IO_ROUNDS; round++) {
		fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
		if (fd < 0)
			_exit(20);
		for (block = 0; block < IO_BLOCKS; block++) {
			fill_block(index, round, block);
			if (write_all(fd, buffer, sizeof(buffer)))
				_exit(21);
		}
		if (fsync(fd) || lseek(fd, 0, SEEK_SET) != 0)
			_exit(22);
		for (block = 0; block < IO_BLOCKS; block++) {
			if (read_all(fd, buffer, sizeof(buffer)) ||
			    verify_block(index, round, block))
				_exit(23);
		}
		if (close(fd))
			_exit(24);
	}
	_exit(0);
}

static void fill_network_block(int worker, int round, int block)
{
	int index;

	for (index = 0; index < BLOCK_SIZE; index++)
		buffer[index] = worker * 31 + round * 13 + block * 11 + index;
}

static void network_worker(int index)
{
	struct sockaddr_in host = {
		.sin_family = AF_INET,
		.sin_port = htons(TCP_BULK_PORT),
	};
	unsigned char expected[BLOCK_SIZE];
	uint32_t network_length = htonl(NET_BLOCKS * BLOCK_SIZE);
	int block, fd, round;

	if (inet_pton(AF_INET, FIXTURE_ADDRESS, &host.sin_addr) != 1)
		_exit(30);
	for (round = 0; round < NET_ROUNDS; round++) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0 ||
		    connect(fd, (const struct sockaddr *)&host,
			    sizeof(host)) < 0 ||
		    write_all(fd, &network_length, sizeof(network_length)))
			_exit(31);
		for (block = 0; block < NET_BLOCKS; block++) {
			fill_network_block(index, round, block);
			if (write_all(fd, buffer, sizeof(buffer)))
				_exit(32);
		}
		for (block = 0; block < NET_BLOCKS; block++) {
			fill_network_block(index, round, block);
			memcpy(expected, buffer, sizeof(expected));
			if (read_all(fd, buffer, sizeof(buffer)) ||
			    memcmp(buffer, expected, sizeof(buffer)))
				_exit(33);
		}
		if (close(fd))
			_exit(34);
	}
	_exit(0);
}

int main(void)
{
	pid_t children[WORKERS];
	int index, status;

	for (index = 0; index < WORKERS; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return fail("fork", index);
		if (!children[index]) {
			if (index < CPU_WORKERS)
				cpu_worker(index);
			if (index < CPU_WORKERS + IO_WORKERS)
				io_worker(index - CPU_WORKERS);
			network_worker(index - CPU_WORKERS - IO_WORKERS);
		}
	}
	for (index = 0; index < WORKERS; index++) {
		status = -1;
		if (waitpid(children[index], &status, 0) != children[index] ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return fail("worker", index * 256 + status);
	}
	puts("PRESSURE_RUNTIME_OK");
	return 0;
}
