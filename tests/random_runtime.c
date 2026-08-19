#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition, name) do { \
	if (!(condition)) { \
		printf("RANDOM_RUNTIME_FAIL %s line=%d errno=%d\n", \
		       (name), __LINE__, errno); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static int all_zero(const unsigned char *buffer, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		if (buffer[index])
			return 0;
	}
	return 1;
}

static void test_fork_stream(void)
{
	unsigned char parent_bytes[32];
	unsigned char *child_bytes;
	int status;
	pid_t child;

	child_bytes = mmap(0, sizeof(parent_bytes), PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(child_bytes != MAP_FAILED, "random shared mapping");
	child = fork();
	CHECK(child >= 0, "random fork");
	if (!child) {
		if (getrandom(child_bytes, sizeof(parent_bytes), 0) !=
		    sizeof(parent_bytes))
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}
	CHECK(getrandom(parent_bytes, sizeof(parent_bytes), 0) ==
	      sizeof(parent_bytes), "random parent stream");
	CHECK(waitpid(child, &status, 0) == child, "random wait");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
	      "random child status");
	CHECK(memcmp(parent_bytes, child_bytes, sizeof(parent_bytes)),
	      "random fork uniqueness");
	CHECK(munmap(child_bytes, sizeof(parent_bytes)) == 0,
	      "random shared unmap");
}

int main(void)
{
	static const uint64_t legacy[2] = {
		0x4341464645494e49ULL,
		0x582d4d55534c2d58ULL,
	};
	unsigned char first[64], second[64];
	const unsigned char *aux_random;

	aux_random = (const unsigned char *)getauxval(AT_RANDOM);
	CHECK(aux_random, "AT_RANDOM address");
	CHECK(!all_zero(aux_random, 16), "AT_RANDOM contents");
	CHECK(memcmp(aux_random, legacy, sizeof(legacy)), "AT_RANDOM legacy");
	CHECK(getrandom(first, sizeof(first), 0) == sizeof(first),
	      "getrandom default");
	CHECK(getrandom(second, sizeof(second), GRND_NONBLOCK) ==
	      sizeof(second), "getrandom nonblock");
	CHECK(!all_zero(first, sizeof(first)), "getrandom contents");
	CHECK(memcmp(first, second, sizeof(first)), "getrandom uniqueness");
	CHECK(getrandom(first, 16, GRND_RANDOM) == 16, "getrandom random");
	CHECK(getrandom(first, 16, GRND_INSECURE) == 16,
	      "getrandom insecure");
	errno = 0;
	CHECK(getrandom(first, 1, GRND_RANDOM | GRND_INSECURE) == -1 &&
	      errno == EINVAL, "getrandom incompatible flags");
	errno = 0;
	CHECK(getrandom(first, 1, 0x80000000U) == -1 && errno == EINVAL,
	      "getrandom invalid flags");
	CHECK(getrandom((void *)-1, 0, 0) == 0, "getrandom zero length");
	errno = 0;
	CHECK(getrandom((void *)-1, 1, 0) == -1 && errno == EFAULT,
	      "getrandom bad address");
	test_fork_stream();
	puts("RANDOM_RUNTIME_OK");
	return EXIT_SUCCESS;
}
