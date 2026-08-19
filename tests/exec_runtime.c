#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int expect_errno(const char *path, char *const argv[], int expected)
{
	errno = 0;
	if (execve(path, argv, (char *const[]){ NULL }) != -1 ||
	    errno != expected) {
		printf("EXEC_RUNTIME_FAIL path=%s errno=%d expected=%d\n",
		       path, errno, expected);
		return -1;
	}
	return 0;
}

int main(void)
{
	char *missing[] = { "/does-not-exist", NULL };
	char *invalid[] = { "/tmp/not-an-elf", NULL };
	char *denied[] = { "/tmp/not-executable", NULL };
	char *too_many[33];
	const char payload[] = "not an executable\n";
	int fd, i;

	if (expect_errno(missing[0], missing, ENOENT) < 0)
		return 1;

	fd = open(invalid[0], O_CREAT | O_TRUNC | O_WRONLY, 0700);
	if (fd < 0 || write(fd, payload, sizeof(payload) - 1) !=
	    (ssize_t)(sizeof(payload) - 1) || close(fd) < 0) {
		printf("EXEC_RUNTIME_FAIL fixture errno=%d\n", errno);
		return 1;
	}
	if (expect_errno(invalid[0], invalid, ENOEXEC) < 0)
		return 1;
	fd = open(denied[0], O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0 || write(fd, payload, sizeof(payload) - 1) !=
	    (ssize_t)(sizeof(payload) - 1) || close(fd) < 0) {
		printf("EXEC_RUNTIME_FAIL mode fixture errno=%d\n", errno);
		return 1;
	}
	if (expect_errno(denied[0], denied, EACCES) < 0)
		return 1;

	for (i = 0; i < 33; i++)
		too_many[i] = (char *)"argument";
	if (expect_errno("/bin/exec-runtime", too_many, E2BIG) < 0)
		return 1;

	puts("EXEC_ERRNO_OK");
	return 0;
}
