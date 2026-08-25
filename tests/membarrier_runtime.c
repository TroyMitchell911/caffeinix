#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/membarrier.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKERS 12
#define BARRIERS 64

static volatile int begin;
static volatile int failure;
static volatile int started;
static volatile uint64_t heartbeat;

static void *worker_main(void *argument)
{
	int i;

	(void)argument;
	__atomic_add_fetch(&started, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&begin, __ATOMIC_ACQUIRE))
		;
	for (i = 0; i < BARRIERS; i++) {
		if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0)) {
			__atomic_store_n(&failure, errno ? errno : 1,
			                 __ATOMIC_RELEASE);
			break;
		}
		__atomic_add_fetch(&heartbeat, 1, __ATOMIC_RELAXED);
	}
	return 0;
}

static int fail(const char *reason, int error)
{
	printf("MEMBARRIER_RUNTIME_FAIL %s error=%d\n", reason, error);
	return 1;
}

static int test_exec_reset(void)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return fail("exec fork", errno);
	if (!child) {
		execl("/bin/membarrier-runtime", "membarrier-runtime",
		      "exec-child", NULL);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("exec reset", status);
	return 0;
}

int main(int argc, char **argv)
{
	pthread_t threads[WORKERS];
	unsigned long spins;
	int commands, error, i;

	if (argc == 2 && !strcmp(argv[1], "exec-child")) {
		errno = 0;
		if (syscall(SYS_membarrier,
		            MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) != -1 ||
		    errno != EPERM)
			return fail("exec registration", errno);
		return 0;
	}
	if (argc != 1)
		return fail("arguments", argc);

	commands = membarrier(MEMBARRIER_CMD_QUERY, 0);
	if ((commands & (MEMBARRIER_CMD_PRIVATE_EXPEDITED |
	                 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)) !=
	    (MEMBARRIER_CMD_PRIVATE_EXPEDITED |
	     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED))
		return fail("query", commands);
	errno = 0;
	if (syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) != -1 ||
	    errno != EPERM)
		return fail("unregistered", errno);
	if (membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0))
		return fail("register", errno);
	errno = 0;
	if (membarrier(MEMBARRIER_CMD_QUERY, 1) != -1 || errno != EINVAL)
		return fail("flags", errno);
	for (i = 0; i < WORKERS; i++) {
		error = pthread_create(&threads[i], 0, worker_main, 0);
		if (error)
			return fail("create", error);
	}
	for (spins = 0; spins < 100000000UL &&
	     __atomic_load_n(&started, __ATOMIC_ACQUIRE) != WORKERS; spins++)
		;
	if (started != WORKERS)
		return fail("start timeout", started);
	__atomic_store_n(&begin, 1, __ATOMIC_RELEASE);
	for (i = 0; i < BARRIERS; i++) {
		if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0))
			return fail("private expedited", errno);
	}
	for (i = 0; i < WORKERS; i++) {
		error = pthread_join(threads[i], 0);
		if (error)
			return fail("join", error);
	}
	if (failure)
		return fail("worker barrier", failure);
	if (heartbeat != (uint64_t)WORKERS * BARRIERS)
		return fail("heartbeat", heartbeat);
	if (test_exec_reset())
		return 1;
	puts("MEMBARRIER_RUNTIME_OK");
	return 0;
}
