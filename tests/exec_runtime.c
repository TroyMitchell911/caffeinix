#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_fixture(const char *path, const char *contents, mode_t mode)
{
	ssize_t length = strlen(contents);
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);

	if (fd < 0 || write(fd, contents, length) != length || close(fd) < 0) {
		printf("EXEC_RUNTIME_FAIL fixture=%s errno=%d\n", path, errno);
		return -1;
	}
	return 0;
}

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

static int test_script(void)
{
	char *arguments[] = { (char *)"ignored-zero", (char *)"payload",
			      NULL };
	int status;
	pid_t child;

	if (write_fixture("/tmp/exec-script",
			  "#!  /bin/exec-runtime  script-option  \n",
			  0700) < 0)
		return -1;
	child = fork();
	if (child < 0) {
		printf("EXEC_RUNTIME_FAIL script fork errno=%d\n", errno);
		return -1;
	}
	if (!child) {
		execve("/tmp/exec-script", arguments,
		       (char *const[]){ NULL });
		printf("EXEC_RUNTIME_FAIL script exec errno=%d\n", errno);
		_exit(1);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status)) {
		printf("EXEC_RUNTIME_FAIL script status=%d errno=%d\n",
		       status, errno);
		return -1;
	}
	return 0;
}

static int test_execfn(void)
{
	char *arguments[] = { (char *)"spoofed-zero",
			      (char *)"execfn-child",
			      (char *)"/bin/exec-runtime", NULL };
	int status;
	pid_t child;

	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		execve(arguments[2], arguments, (char *const[]){ NULL });
		_exit(127);
	}
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       !WEXITSTATUS(status) ? 0 : -1;
}

static int test_truncated_script_interpreter(void)
{
	char interpreter[255];
	char line[272];
	char *arguments[] = { (char *)"/tmp/exec-script-truncated", NULL };
	pid_t child;
	int result = -1, status;

	memmove(interpreter, "/tmp/", 5);
	memset(interpreter + 5, 'i', 249);
	interpreter[254] = 0;
	line[0] = '#';
	line[1] = '!';
	memmove(line + 2, interpreter, 254);
	memmove(line + 256, "-suffix\n", 9);
	if (symlink("/bin/false", interpreter) ||
	    write_fixture(arguments[0], line, 0700) < 0)
		goto out;
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		execve(arguments[0], arguments, (char *const[]){ NULL });
		_exit(errno == ENOEXEC ? 0 : 1);
	}
	if (waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    !WEXITSTATUS(status))
		result = 0;
out:
	unlink(arguments[0]);
	unlink(interpreter);
	return result;
}

static int test_script_recursion(void)
{
	char path[32], next[32], line[80];
	char *arguments[] = { path, NULL };
	int i;

	for (i = 0; i < 5; i++) {
		snprintf(path, sizeof(path), "/tmp/exec-script-%d", i);
		snprintf(next, sizeof(next), "/tmp/exec-script-%d", i + 1);
		snprintf(line, sizeof(line), "#!%s\n", next);
		if (write_fixture(path, line, 0700) < 0)
			return -1;
	}
	snprintf(path, sizeof(path), "/tmp/exec-script-5");
	if (write_fixture(path, "#!/bin/exec-runtime script-option\n",
			  0700) < 0)
		return -1;
	snprintf(path, sizeof(path), "/tmp/exec-script-0");
	return expect_errno(path, arguments, ELOOP);
}

static int long_argument_child(int argc, char **argv)
{
	char snapshot[4097];
	ssize_t count, total = 0;
	int fd, index;

	if (argc != 8)
		return -1;
	for (index = 2; index < argc; index++)
		if (strlen(argv[index]) != 1024)
			return -1;
	fd = open("/proc/self/cmdline", O_RDONLY);
	if (fd < 0)
		return -1;
	while (total < (ssize_t)sizeof(snapshot)) {
		count = read(fd, snapshot + total, sizeof(snapshot) - total);
		if (count < 0) {
			close(fd);
			return -1;
		}
		if (!count)
			break;
		total += count;
	}
	if (close(fd) || total != 4096 || snapshot[4095])
		return -1;
	return 0;
}

static int test_long_arguments(void)
{
	char argument[1025];
	char *arguments[9] = {
		(char *)"/bin/exec-runtime", (char *)"long-child",
		argument, argument, argument, argument, argument, argument, NULL,
	};
	pid_t child;
	int status;

	memset(argument, 'x', sizeof(argument) - 1);
	argument[sizeof(argument) - 1] = 0;
	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		execv(arguments[0], arguments);
		_exit(127);
	}
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       !WEXITSTATUS(status) ? 0 : -1;
}

int main(int argc, char **argv)
{
	char *missing[] = { "/does-not-exist", NULL };
	char *invalid[] = { "/tmp/not-an-elf", NULL };
	char *denied[] = { "/tmp/not-executable", NULL };
	char *too_many[33];
	const char payload[] = "not an executable\n";
	int i;

	if (argc > 1 && !strcmp(argv[1], "execfn-child")) {
		const char *execfn = (const char *)getauxval(AT_EXECFN);

		if (argc != 3 || !execfn || strcmp(execfn, argv[2])) {
			puts("EXEC_RUNTIME_FAIL direct execfn");
			return 1;
		}
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "long-child")) {
		if (long_argument_child(argc, argv)) {
			puts("EXEC_RUNTIME_FAIL long child");
			return 1;
		}
		return 0;
	}

	if (argc > 1 && !strcmp(argv[1], "script-option")) {
		const char *execfn = (const char *)getauxval(AT_EXECFN);
		char name[16] = { 0 };

		if (prctl(PR_GET_NAME, name) || strcmp(name, "exec-script") ||
		    argc != 4 || strcmp(argv[0], "/bin/exec-runtime") ||
		    strcmp(argv[2], "/tmp/exec-script") ||
		    strcmp(argv[3], "payload") || !execfn ||
		    strcmp(execfn, "/tmp/exec-script")) {
			printf("EXEC_RUNTIME_FAIL script argv argc=%d\n", argc);
			return 1;
		}
		puts("EXEC_SCRIPT_CHILD_OK");
		return 0;
	}

	if (expect_errno(missing[0], missing, ENOENT) < 0)
		return 1;

	if (write_fixture(invalid[0], payload, 0700) < 0)
		return 1;
	if (expect_errno(invalid[0], invalid, ENOEXEC) < 0)
		return 1;
	if (write_fixture(denied[0], payload, 0600) < 0)
		return 1;
	if (expect_errno(denied[0], denied, EACCES) < 0)
		return 1;

	for (i = 0; i < 33; i++)
		too_many[i] = (char *)"argument";
	if (expect_errno("/bin/exec-runtime", too_many, E2BIG) < 0)
		return 1;
	if (test_execfn() < 0 || test_script() < 0 ||
	    test_truncated_script_interpreter() < 0 ||
	    test_script_recursion() < 0 || test_long_arguments() < 0)
		return 1;

	puts("EXEC_ERRNO_OK");
	puts("EXEC_SCRIPT_OK");
	return 0;
}
