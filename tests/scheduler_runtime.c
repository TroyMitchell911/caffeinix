#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_ROUNDS 32
#define EXEC_SMOKE_ROUNDS 8
#define RUNNABLE_CHILDREN 24
#define RUNNABLE_SMOKE_CHILDREN 12
#define MIXED_CHILDREN 16
#define TTY_WAITERS 8

static int last_wait_status;

static int fail(const char *test, int code)
{
	char message[128];
	int length;

	length = snprintf(message, sizeof(message),
	                  "SCHED_RUNTIME_FAIL=%s:%d errno=%d status=%d\n",
	                  test, code, errno, last_wait_status);
	write(1, message, length);
	return code;
}

static void pass(const char *message)
{
	write(1, message, strlen(message));
}

static int wait_for_child(pid_t pid, int expected)
{
	int status;

	last_wait_status = -1;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	last_wait_status = status;
	if (!WIFEXITED(status) ||
	    WEXITSTATUS(status) != expected)
		return -1;
	return 0;
}

static int test_exec(int rounds)
{
	int round;

	for (round = 0; round < rounds; round++) {
		pid_t pid = fork();

		if (pid < 0)
			return fail("exec-fork", 10);
		if (!pid) {
			execl("/bin/scheduler-runtime", "scheduler-runtime",
			      "exec-child", NULL);
			_exit(127);
		}
		if (wait_for_child(pid, 0))
			return fail("exec-wait", 11);
	}
	pass("SCHED_EXEC_OK\n");
	return 0;
}

static void runnable_child(int index)
{
	volatile uint64_t value = index + 1;
	int iteration;

	for (iteration = 0; iteration < 1000000; iteration++)
		value = value * 6364136223846793005ULL + 1;
	_exit(value == UINT64_MAX ? 101 : index);
}

static int test_runqueue(int count)
{
	pid_t children[RUNNABLE_CHILDREN];
	int done[RUNNABLE_CHILDREN] = { 0 };
	int index, remaining = count;

	for (index = 0; index < count; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return fail("runqueue-fork", 20);
		if (!children[index])
			runnable_child(index);
	}
	while (remaining) {
		for (index = 0; index < count; index++) {
			pid_t result;
			int status;

			if (done[index])
				continue;
			result = waitpid(children[index], &status, WNOHANG);
			if (!result)
				continue;
			last_wait_status = status;
			if (result != children[index] || !WIFEXITED(status) ||
			    WEXITSTATUS(status) != index)
				return fail("runqueue-wait", 21);
			done[index] = 1;
			remaining--;
		}
	}
	pass("SCHED_RUNQUEUE_OK\n");
	return 0;
}

static int read_busybox(void)
{
	char buffer[1024];
	ssize_t count;
	int fd = open("/bin/busybox", O_RDONLY);

	if (fd < 0)
		return -1;
	while ((count = read(fd, buffer, sizeof(buffer))) > 0)
		;
	if (count < 0 || close(fd))
		return -1;
	return 0;
}

static void mixed_child(int index)
{
	volatile uint64_t value = index + 1;
	char *allocation;
	int iteration;

	allocation = malloc(65536);
	if (!allocation)
		_exit(110);
	memset(allocation, index, 65536);
	if (index & 1) {
		if (read_busybox())
			_exit(111);
	} else {
		for (iteration = 0; iteration < 400000; iteration++)
			value = value * 2862933555777941757ULL + 3037000493ULL;
	}
	free(allocation);
	_exit(value == UINT64_MAX ? 112 : index);
}

static int test_mixed(void)
{
	pid_t children[MIXED_CHILDREN];
	int index;

	for (index = 0; index < MIXED_CHILDREN; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return fail("mixed-fork", 30);
		if (!children[index])
			mixed_child(index);
	}
	for (index = 0; index < MIXED_CHILDREN; index++)
		if (wait_for_child(children[index], index))
			return fail("mixed-wait", 31);
	pass("SCHED_MIXED_OK\n");
	return 0;
}

static void tty_waiter(void)
{
	char input[32];
	int fd = open("/dev/ttyS0", O_RDONLY);

	if (fd < 0)
		_exit(120);
	pass("SCHED_TTY_READY\n");
	if (read(fd, input, sizeof(input)) <= 0 || close(fd))
		_exit(121);
	_exit(0);
}

static int test_tty_wait(void)
{
	pid_t children[TTY_WAITERS];
	int index;

	for (index = 0; index < TTY_WAITERS; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return fail("tty-fork", 40);
		if (!children[index])
			tty_waiter();
	}
	for (index = 0; index < TTY_WAITERS; index++)
		if (wait_for_child(children[index], 0))
			return fail("tty-wait", 41);
	pass("SCHED_TTY_WAIT_OK\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "exec-child"))
		return 0;
	if (argc == 2 && !strcmp(argv[1], "tty-wait"))
		return test_tty_wait();
	if (argc == 2 && !strcmp(argv[1], "smoke")) {
		if (test_exec(EXEC_SMOKE_ROUNDS) ||
		    test_runqueue(RUNNABLE_SMOKE_CHILDREN))
			return 1;
		pass("SCHED_SMOKE_OK\n");
		return 0;
	}
	if (argc != 1)
		return fail("arguments", 1);
	if (test_exec(EXEC_ROUNDS) ||
	    test_runqueue(RUNNABLE_CHILDREN) || test_mixed())
		return 1;
	pass("SCHED_RUNTIME_OK\n");
	return 0;
}
