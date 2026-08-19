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

static void test_pread(void)
{
	unsigned char buffer[32];
	off_t position = 17;
	int fd;

	fd = open(FILE_PATH, O_RDONLY);
	CHECK(fd >= 0, "pread open");
	CHECK(lseek(fd, position, SEEK_SET) == position, "pread seek");
	CHECK(pread(fd, buffer, sizeof(buffer), PAGE_SIZE + 19) ==
	      (ssize_t)sizeof(buffer), "pread data");
	for (size_t index = 0; index < sizeof(buffer); index++)
		CHECK(buffer[index] == pattern(1, index + 19),
		      "pread contents");
	CHECK(lseek(fd, 0, SEEK_CUR) == position, "pread position");
	errno = 0;
	CHECK(pread(fd, buffer, sizeof(buffer), -1) == -1 &&
	      errno == EINVAL, "pread negative offset");
	CHECK(close(fd) == 0, "pread close");
	errno = 0;
	CHECK(pread(fd, buffer, sizeof(buffer), 0) == -1 && errno == EBADF,
	      "pread closed descriptor");
	puts("VM_PREAD_OK");
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
	test_pread();
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
