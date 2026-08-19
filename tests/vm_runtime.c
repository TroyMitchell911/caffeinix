#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_SIZE 4096UL
#define FILE_PATH "/tmp/vm-runtime.bin"
#define SHARED_READ_PATH "/vm-shared-read.bin"
#define SHARED_READ_START "/tmp/vm-shared-read-start"
#define SHARED_READ_CHILDREN 8
#define HINT_ADDRESS ((void *)0x20000000UL)

#define CHECK(condition, name) do { \
	if (!(condition)) \
		fail((name), __LINE__); \
} while (0)

static void fail(const char *name, int line)
{
	printf("VM_RUNTIME_FAIL %s line=%d errno=%d\n", name, line, errno);
	exit(EXIT_FAILURE);
}

static unsigned char pattern(unsigned int page, unsigned int index)
{
	return (unsigned char)(page * 37U + index * 13U + 11U);
}

static void fill_page(unsigned char *buffer, unsigned int page)
{
	unsigned int index;

	for (index = 0; index < PAGE_SIZE; index++)
		buffer[index] = pattern(page, index);
}

static void check_page(const unsigned char *buffer, unsigned int page)
{
	unsigned int index;

	for (index = 0; index < PAGE_SIZE; index++)
		CHECK(buffer[index] == pattern(page, index), "file contents");
}

static void write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;

	while (length) {
		ssize_t written = write(fd, bytes, length);

		CHECK(written > 0, "write fixture");
		bytes += written;
		length -= written;
	}
}

static void create_fixture(void)
{
	unsigned char buffer[PAGE_SIZE];
	int fd = open(FILE_PATH, O_CREAT | O_TRUNC | O_RDWR, 0600);

	CHECK(fd >= 0, "open fixture");
	fill_page(buffer, 0);
	write_all(fd, buffer, sizeof(buffer));
	fill_page(buffer, 1);
	write_all(fd, buffer, sizeof(buffer));
	CHECK(close(fd) == 0, "close fixture");
}

static void expect_read_fault(int fd, void *destination,
			      const char *name)
{
	errno = 0;
	CHECK(read(fd, destination, 1) == -1 && errno == EFAULT, name);
}

static void test_read_copy_faults(void)
{
	static const char value = 'x';
	void *destination;
	int fd;

	destination = mmap(0, PAGE_SIZE, PROT_READ,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(destination != MAP_FAILED, "read fault mmap");

	fd = open(FILE_PATH, O_RDONLY);
	CHECK(fd >= 0, "read fault tmpfs open");
	expect_read_fault(fd, destination, "read fault tmpfs");
	CHECK(close(fd) == 0, "read fault tmpfs close");

	fd = open("/bin/sh", O_RDONLY);
	CHECK(fd >= 0, "read fault ext4 open");
	expect_read_fault(fd, destination, "read fault ext4");
	CHECK(close(fd) == 0, "read fault ext4 close");

	fd = open("/mnt/fat/read-fault", O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0, "read fault FAT open");
	write_all(fd, &value, sizeof(value));
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "read fault FAT seek");
	expect_read_fault(fd, destination, "read fault FAT");
	CHECK(close(fd) == 0, "read fault FAT close");
	CHECK(unlink("/mnt/fat/read-fault") == 0, "read fault FAT unlink");

	fd = open("/dev/zero", O_RDONLY);
	CHECK(fd >= 0, "read fault zero open");
	expect_read_fault(fd, destination, "read fault zero");
	CHECK(close(fd) == 0, "read fault zero close");

	CHECK(munmap(destination, PAGE_SIZE) == 0, "read fault munmap");
	puts("VM_READ_FAULT_OK");
}

static void shared_read_child(int fd)
{
	unsigned char buffer[PAGE_SIZE];
	unsigned int index, page;

	while (access(SHARED_READ_START, F_OK) < 0) {
		if (errno != ENOENT)
			_exit(1);
	}
	if (read(fd, buffer, sizeof(buffer)) != sizeof(buffer))
		_exit(1);
	for (page = 0; page < SHARED_READ_CHILDREN; page++) {
		if (buffer[0] == pattern(page, 0))
			break;
	}
	if (page == SHARED_READ_CHILDREN)
		_exit(1);
	for (index = 0; index < PAGE_SIZE; index++) {
		if (buffer[index] != pattern(page, index))
			_exit(1);
	}
	_exit(16 + page);
}

static void test_shared_file_reads(void)
{
	unsigned char buffer[PAGE_SIZE];
	pid_t children[SHARED_READ_CHILDREN];
	unsigned char seen[SHARED_READ_CHILDREN] = { 0 };
	unsigned int index;
	int fd, start_fd, status;

	fd = open(SHARED_READ_PATH, O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0, "shared read open");
	for (index = 0; index < SHARED_READ_CHILDREN; index++) {
		fill_page(buffer, index);
		write_all(fd, buffer, sizeof(buffer));
	}
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "shared read seek");
	unlink(SHARED_READ_START);
	for (index = 0; index < SHARED_READ_CHILDREN; index++) {
		children[index] = fork();
		CHECK(children[index] >= 0, "shared read fork");
		if (!children[index])
			shared_read_child(fd);
	}
	start_fd = open(SHARED_READ_START, O_CREAT | O_TRUNC | O_WRONLY,
			0600);
	CHECK(start_fd >= 0, "shared read barrier");
	CHECK(close(start_fd) == 0, "shared read barrier close");
	for (index = 0; index < SHARED_READ_CHILDREN; index++) {
		int page;

		CHECK(waitpid(children[index], &status, 0) == children[index],
		      "shared read wait");
		CHECK(WIFEXITED(status), "shared read child exit");
		page = WEXITSTATUS(status) - 16;
		CHECK(page >= 0 && page < SHARED_READ_CHILDREN,
		      "shared read child status");
		CHECK(!seen[page], "shared read duplicate page");
		seen[page] = 1;
	}
	CHECK(close(fd) == 0, "shared read close");
	CHECK(unlink(SHARED_READ_START) == 0, "shared read barrier unlink");
	CHECK(unlink(SHARED_READ_PATH) == 0, "shared read unlink");
	puts("VM_SHARED_READ_OK");
}

static void test_file_mapping(unsigned char **mapping_out)
{
	unsigned char buffer[PAGE_SIZE];
	unsigned char *mapping;
	int fd = open(FILE_PATH, O_RDONLY);

	CHECK(fd >= 0, "open mapped file");
	mapping = mmap(0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE, fd, PAGE_SIZE);
	CHECK(mapping != MAP_FAILED, "file mmap");
	CHECK(close(fd) == 0, "close mapped fd");
	check_page(mapping, 1);
	for (size_t index = PAGE_SIZE; index < 2 * PAGE_SIZE; index++)
		CHECK(mapping[index] == 0, "file zero tail");
	mapping[0] ^= 0xff;
	fd = open(FILE_PATH, O_RDONLY);
	CHECK(fd >= 0, "reopen mapped file");
	CHECK(lseek(fd, PAGE_SIZE, SEEK_SET) == PAGE_SIZE, "seek fixture");
	CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer),
	      "read fixture");
	CHECK(buffer[0] == pattern(1, 0), "private file write");
	CHECK(close(fd) == 0, "close reopened file");
	*mapping_out = mapping;
}

static unsigned char *test_anonymous_mapping(void)
{
	unsigned char *mapping;

	mapping = mmap(0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(mapping != MAP_FAILED, "anonymous mmap");
	for (size_t index = 0; index < 3 * PAGE_SIZE; index++)
		CHECK(mapping[index] == 0, "anonymous zero fill");
	mapping[0] = 0x21;
	mapping[PAGE_SIZE] = 0x32;
	mapping[2 * PAGE_SIZE] = 0x43;
	return mapping;
}

static void wait_for_mapping_fault(pid_t child, const char *name)
{
	int status;

	CHECK(child > 0, "fork protected mapping");
	CHECK(waitpid(child, &status, 0) == child, "wait protected mapping");
	CHECK(!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS, name);
}

static void test_hint_and_fixed(void)
{
	unsigned char *mapping;

	mapping = mmap(HINT_ADDRESS, PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(mapping == HINT_ADDRESS, "mmap hint");
	CHECK(munmap(mapping, PAGE_SIZE) == 0, "unmap hint");

	mapping = mmap(HINT_ADDRESS, 2 * PAGE_SIZE,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	CHECK(mapping == HINT_ADDRESS, "fixed mmap");
	mapping[0] = 0x51;
	mapping[PAGE_SIZE] = 0x52;
	CHECK(mmap(mapping + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
	      mapping + PAGE_SIZE, "fixed replacement");
	CHECK(mapping[0] == 0x51 && mapping[PAGE_SIZE] == 0,
	      "fixed replacement contents");
	CHECK(munmap(mapping, 2 * PAGE_SIZE) == 0, "unmap fixed");

	errno = 0;
	CHECK(mmap((char *)HINT_ADDRESS + 1, PAGE_SIZE,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
	      MAP_FAILED && errno == EINVAL, "unaligned fixed mmap");
}

static void test_partial_changes(unsigned char *mapping)
{
	unsigned char *middle = mapping + PAGE_SIZE;
	void *replacement;
	pid_t child;

	CHECK(mprotect(middle, PAGE_SIZE, PROT_NONE) == 0,
	      "partial PROT_NONE");
	child = fork();
	CHECK(child >= 0, "fork PROT_NONE");
	if (!child) {
		volatile unsigned char value =
			*(volatile unsigned char *)middle;

		(void)value;
		_exit(EXIT_SUCCESS);
	}
	wait_for_mapping_fault(child, "PROT_NONE fault status");
	CHECK(mprotect(middle, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
	      "restore protection");
	CHECK(middle[0] == 0x32, "PROT_NONE preserved data");
	CHECK(mprotect(middle, PAGE_SIZE, PROT_READ) == 0,
	      "partial read protection");
	child = fork();
	CHECK(child >= 0, "fork read-only mapping");
	if (!child) {
		*(volatile unsigned char *)middle = 0xff;
		_exit(EXIT_SUCCESS);
	}
	wait_for_mapping_fault(child, "read-only fault status");
	CHECK(mprotect(middle, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
	      "restore write protection");

	CHECK(munmap(middle, PAGE_SIZE) == 0, "partial munmap");
	errno = 0;
	CHECK(mprotect(middle, PAGE_SIZE, PROT_READ) < 0 && errno == ENOMEM,
	      "protect unmapped hole");
	replacement = mmap(middle, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(replacement == middle, "reuse partial hole");
	CHECK(middle[0] == 0, "reused hole zero fill");
}

static void test_fork_isolation(unsigned char *anonymous,
				unsigned char *file_mapping)
{
	unsigned char anonymous_value = anonymous[0];
	unsigned char file_value = file_mapping[0];
	int status;
	pid_t child = fork();

	CHECK(child >= 0, "fork mappings");
	if (!child) {
		anonymous[0] ^= 0xff;
		file_mapping[0] ^= 0xff;
		_exit(EXIT_SUCCESS);
	}
	CHECK(waitpid(child, &status, 0) == child, "wait mappings");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
	      "child mapping status");
	CHECK(anonymous[0] == anonymous_value, "anonymous fork isolation");
	CHECK(file_mapping[0] == file_value, "file fork isolation");
}

static void test_mapping_lifetime(void)
{
	int iteration;

	for (iteration = 0; iteration < 256; iteration++) {
		unsigned char *mapping;
		int fd = open(FILE_PATH, O_RDONLY);

		CHECK(fd >= 0, "lifetime open");
		mapping = mmap(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
		CHECK(mapping != MAP_FAILED, "lifetime mmap");
		CHECK(close(fd) == 0, "lifetime close");
		CHECK(mapping[0] == pattern(0, 0), "lifetime contents");
		CHECK(munmap(mapping, PAGE_SIZE) == 0, "lifetime munmap");
	}
}

static void test_kernel_copy_permissions(void)
{
	struct iovec *iov;
	unsigned char *mapping;
	int fd, null_fd;

	mapping = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(mapping != MAP_FAILED, "copy permission mmap");
	fd = open(FILE_PATH, O_RDONLY);
	CHECK(fd >= 0, "copy permission file");
	CHECK(mprotect(mapping, PAGE_SIZE, PROT_READ) == 0,
	      "copy permission read-only");
	errno = 0;
	CHECK(syscall(SYS_fstat, fd, mapping) < 0 && errno == EFAULT,
	      "copyout write permission");
	CHECK(mapping[0] == 0, "copyout preserved mapping");
	CHECK(mprotect(mapping, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
	      "copy permission restore write");
	iov = (struct iovec *)mapping;
	iov->iov_base = (void *)"x";
	iov->iov_len = 1;
	null_fd = open("/dev/null", O_WRONLY);
	CHECK(null_fd >= 0, "copy permission null");
	CHECK(mprotect(mapping, PAGE_SIZE, PROT_EXEC) == 0,
	      "copy permission execute-only");
	errno = 0;
	CHECK(syscall(SYS_writev, null_fd, iov, 1) < 0 && errno == EFAULT,
	      "copyin read permission");
	CHECK(mprotect(mapping, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
	      "copy permission restore read");
	CHECK(close(null_fd) == 0, "copy permission close null");
	CHECK(close(fd) == 0, "copy permission close file");
	CHECK(munmap(mapping, PAGE_SIZE) == 0, "copy permission munmap");
}

int main(void)
{
	unsigned char *anonymous, *file_mapping;

	create_fixture();
	test_read_copy_faults();
	test_shared_file_reads();
	test_file_mapping(&file_mapping);
	anonymous = test_anonymous_mapping();
	test_hint_and_fixed();
	test_partial_changes(anonymous);
	test_fork_isolation(anonymous, file_mapping);
	test_mapping_lifetime();
	test_kernel_copy_permissions();
	CHECK(munmap(anonymous, 3 * PAGE_SIZE) == 0, "unmap anonymous");
	CHECK(munmap(file_mapping, 2 * PAGE_SIZE) == 0, "unmap file");
	CHECK(unlink(FILE_PATH) == 0, "unlink fixture");
	puts("VM_RUNTIME_OK");
	return EXIT_SUCCESS;
}
