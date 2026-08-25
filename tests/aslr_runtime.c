#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SAMPLE_COUNT 8
#define PAGE_SIZE 4096

#define CHECK(condition, name) do { \
	if (!(condition)) { \
		printf("ASLR_RUNTIME_FAIL %s line=%d errno=%d\n", \
		       (name), __LINE__, errno); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

extern int dynamic_fixture_value(void);

struct address_sample {
	uintptr_t executable;
	uintptr_t interpreter;
	uintptr_t library;
	uintptr_t mapping;
	uintptr_t heap;
	uintptr_t stack;
};

static void write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;

	while (length) {
		ssize_t count = write(fd, bytes, length);

		CHECK(count > 0, "sample write");
		bytes += count;
		length -= count;
	}
}

static void read_all(int fd, void *buffer, size_t length)
{
	unsigned char *bytes = buffer;

	while (length) {
		ssize_t count = read(fd, bytes, length);

		CHECK(count > 0, "sample read");
		bytes += count;
		length -= count;
	}
}

static void emit_sample(const char *path)
{
	struct address_sample sample;
	unsigned char stack_byte;
	void *mapping;
	int fd;

	mapping = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(mapping != MAP_FAILED, "sample mmap");
	CHECK(dynamic_fixture_value() == 17, "sample library");
	sample.executable = (uintptr_t)&emit_sample;
	sample.interpreter = getauxval(AT_BASE);
	sample.library = (uintptr_t)&dynamic_fixture_value;
	sample.mapping = (uintptr_t)mapping;
	sample.heap = (uintptr_t)sbrk(0);
	sample.stack = (uintptr_t)&stack_byte;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0, "sample open");
	write_all(fd, &sample, sizeof(sample));
	CHECK(close(fd) == 0, "sample close");
	CHECK(munmap(mapping, PAGE_SIZE) == 0, "sample unmap");
}

static void collect_sample(unsigned int index,
			   struct address_sample *sample)
{
	char path[64];
	int fd, status;
	pid_t child;

	CHECK(snprintf(path, sizeof(path), "/tmp/aslr-%u", index) > 0,
	      "sample path");
	(void)unlink(path);
	child = fork();
	CHECK(child >= 0, "sample fork");
	if (!child) {
		execl("/bin/aslr-runtime", "aslr-runtime", "sample", path,
		      (char *)0);
		_exit(EXIT_FAILURE);
	}
	CHECK(waitpid(child, &status, 0) == child, "sample wait");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
	      "sample status");
	fd = open(path, O_RDONLY);
	CHECK(fd >= 0, "sample reopen");
	read_all(fd, sample, sizeof(*sample));
	CHECK(close(fd) == 0, "sample read close");
	CHECK(unlink(path) == 0, "sample unlink");
}

static int differs(const struct address_sample *first,
		   const struct address_sample *sample, size_t offset)
{
	const uintptr_t *left = (const uintptr_t *)
		((const unsigned char *)first + offset);
	const uintptr_t *right = (const uintptr_t *)
		((const unsigned char *)sample + offset);

	return *left != *right;
}

static void test_aslr(void)
{
	struct address_sample samples[SAMPLE_COUNT];
	unsigned int changed[6] = {0};
	size_t offsets[] = {
		offsetof(struct address_sample, executable),
		offsetof(struct address_sample, interpreter),
		offsetof(struct address_sample, library),
		offsetof(struct address_sample, mapping),
		offsetof(struct address_sample, heap),
		offsetof(struct address_sample, stack),
	};

	for (unsigned int i = 0; i < SAMPLE_COUNT; i++) {
		const uintptr_t *values = (const uintptr_t *)&samples[i];

		collect_sample(i, &samples[i]);
		for (size_t field = 0; field < sizeof(offsets) / sizeof(offsets[0]);
		     field++)
			CHECK(values[field] && values[field] < 0x40000000UL,
			      "sample address range");
		CHECK(!(samples[i].interpreter & (PAGE_SIZE - 1)),
		      "interpreter alignment");
		CHECK(!(samples[i].mapping & (PAGE_SIZE - 1)),
		      "mapping alignment");
		if (!i)
			continue;
		for (size_t field = 0; field < sizeof(offsets) / sizeof(offsets[0]);
		     field++)
			changed[field] |= differs(&samples[0], &samples[i],
						  offsets[field]);
	}
	for (size_t field = 0; field < sizeof(changed) / sizeof(changed[0]);
	     field++)
		CHECK(changed[field], "address did not change");
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "sample")) {
		emit_sample(argv[2]);
		return EXIT_SUCCESS;
	}
	CHECK(argc == 1, "arguments");
	test_aslr();
	puts("ASLR_RUNTIME_OK");
	return EXIT_SUCCESS;
}
