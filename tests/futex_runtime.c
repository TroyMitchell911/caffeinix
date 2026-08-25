#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

struct waiter {
	volatile int *address;
	int operation;
	uint32_t bitset;
};

static volatile int source;
static volatile int destination;

static int fail(const char *reason, long value)
{
	printf("FUTEX_RUNTIME_FAIL %s value=%ld errno=%d\n",
	       reason, value, errno);
	return 1;
}

static void *waiter_main(void *argument)
{
	struct waiter *waiter = argument;
	long result;

	result = syscall(SYS_futex, waiter->address, waiter->operation,
	                 0, 0, 0, waiter->bitset);
	return (void *)(intptr_t)(result == 0 ? 0 : errno);
}

static int wake_until(volatile int *address, int operation,
		      int count, uint32_t bitset)
{
	unsigned long attempts;
	long result;

	for (attempts = 0; attempts < 1000000UL; attempts++) {
		result = syscall(SYS_futex, address, operation, count,
		                 0, 0, bitset);
		if (result)
			return result;
	}
	return 0;
}

static int test_bitset(void)
{
	pthread_t threads[2];
	struct waiter waiters[2] = {
		{ &source, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 1 },
		{ &source, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 2 },
	};
	void *result;
	int i;

	source = 0;
	for (i = 0; i < 2; i++) {
		if (pthread_create(&threads[i], 0, waiter_main,
		                   &waiters[i]))
			return fail("bitset create", i);
	}
	if (wake_until(&source,
	               FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG,
	               1, 1) != 1)
		return fail("bitset one", 0);
	if (wake_until(&source,
	               FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG,
	               1, 2) != 1)
		return fail("bitset two", 0);
	for (i = 0; i < 2; i++) {
		pthread_join(threads[i], &result);
		if ((intptr_t)result)
			return fail("bitset join", (intptr_t)result);
	}
	return 0;
}

static int test_requeue(void)
{
	pthread_t threads[2];
	struct waiter waiters[2] = {
		{ &source, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
		  FUTEX_BITSET_MATCH_ANY },
		{ &source, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
		  FUTEX_BITSET_MATCH_ANY },
	};
	void *result;
	unsigned long attempts;
	long moved, total = 0;
	int i;

	source = destination = 0;
	for (i = 0; i < 2; i++) {
		if (pthread_create(&threads[i], 0, waiter_main,
		                   &waiters[i]))
			return fail("requeue create", i);
	}
	for (attempts = 0; attempts < 1000000UL && total < 2; attempts++) {
		moved = syscall(SYS_futex, &source,
		                FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG,
		                0, (void *)(uintptr_t)(2 - total),
		                &destination, 0);
		if (moved < 0)
			return fail("requeue", moved);
		total += moved;
	}
	if (total != 2)
		return fail("requeue count", total);
	if (syscall(SYS_futex, &source,
	            FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 2) != 0)
		return fail("requeue source wake", 0);
	if (syscall(SYS_futex, &destination,
	            FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 2) != 2)
		return fail("requeue destination wake", 0);
	for (i = 0; i < 2; i++) {
		pthread_join(threads[i], &result);
		if ((intptr_t)result)
			return fail("requeue join", (intptr_t)result);
	}
	return 0;
}

static int test_shared(void)
{
	pthread_t thread;
	struct waiter waiter = {
		&source, FUTEX_WAIT, FUTEX_BITSET_MATCH_ANY,
	};
	void *result;

	source = 0;
	if (pthread_create(&thread, 0, waiter_main, &waiter))
		return fail("shared create", 0);
	if (wake_until(&source, FUTEX_WAKE, 1,
	               FUTEX_BITSET_MATCH_ANY) != 1)
		return fail("shared wake", 0);
	pthread_join(thread, &result);
	if ((intptr_t)result)
		return fail("shared join", (intptr_t)result);
	return 0;
}

static int test_timeouts(void)
{
	struct timespec timeout = { .tv_nsec = 2000000 };
	struct timespec absolute;
	long result;

	source = 0;
	errno = 0;
	result = syscall(SYS_futex, &source,
	                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
	                 0, &timeout);
	if (result != -1 || errno != ETIMEDOUT)
		return fail("relative timeout", result);
	clock_gettime(CLOCK_MONOTONIC, &absolute);
	absolute.tv_nsec += 2000000;
	if (absolute.tv_nsec >= 1000000000L) {
		absolute.tv_sec++;
		absolute.tv_nsec -= 1000000000L;
	}
	errno = 0;
	result = syscall(SYS_futex, &source,
	                 FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
	                 0, &absolute, 0, 1);
	if (result != -1 || errno != ETIMEDOUT)
		return fail("absolute timeout", result);
	errno = 0;
	result = syscall(SYS_futex, &source,
	                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
	                 1, 0, 0, 0);
	if (result != -1 || errno != EAGAIN)
		return fail("compare mismatch", result);
	errno = 0;
	result = syscall(SYS_futex, &source,
	                 FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
	                 0, 0, 0, 0);
	if (result != -1 || errno != EINVAL)
		return fail("empty bitset", result);
	return 0;
}

int main(void)
{
	if (test_bitset() || test_requeue() || test_shared() ||
	    test_timeouts())
		return 1;
	puts("FUTEX_RUNTIME_OK");
	return 0;
}
