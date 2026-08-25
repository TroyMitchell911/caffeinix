#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *operation)
{
	fprintf(stderr, "MEMORY_RUNTIME_FAIL %s errno=%d\n", operation,
		errno);
	return 1;
}

static int run_hold(const char *ready_path, const char *release_path)
{
	int descriptor;

	descriptor = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (descriptor < 0)
		return fail("ready open");
	if (close(descriptor) < 0)
		return fail("ready close");
	printf("MEMORY_HOLD_READY %s\n", ready_path);
	fflush(stdout);
	while (access(release_path, F_OK) < 0) {
		if (errno != ENOENT)
			return fail("release access");
		if (poll(0, 0, 1) < 0 && errno != EINTR)
			return fail("release poll");
	}
	if (unlink(ready_path) < 0 && errno != ENOENT)
		return fail("ready unlink");
	printf("MEMORY_HOLD_DONE %s\n", ready_path);
	return 0;
}

static int run_fork(const char *argument)
{
	char *end;
	long count, index;

	errno = 0;
	count = strtol(argument, &end, 10);
	if (errno || *argument == '\0' || *end != '\0' || count < 1 ||
	    count > 1024) {
		errno = EINVAL;
		return fail("fork count");
	}
	for (index = 0; index < count; index++) {
		pid_t child = fork();
		int status;

		if (child < 0)
			return fail("fork");
		if (!child)
			_exit(0);
		if (waitpid(child, &status, 0) != child)
			return fail("waitpid");
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			errno = ECHILD;
			return fail("child status");
		}
	}
	printf("MEMORY_FORK_OK %ld\n", count);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "once")) {
		puts("MEMORY_ONCE_OK");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "hold"))
		return run_hold(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "fork"))
		return run_fork(argv[2]);
	fputs("usage: memory-runtime once|hold READY RELEASE|fork COUNT\n",
	      stderr);
	return 2;
}
