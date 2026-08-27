#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_BRK_LIMIT 0x3efbf000ULL
#define EXEC_PHDR_MAX  32

static int copy_file_contents(int input, int output)
{
	char buffer[4096];
	ssize_t count, written;

	while ((count = read(input, buffer, sizeof(buffer))) > 0) {
		written = 0;
		while (written < count) {
			ssize_t result = write(output, buffer + written,
					       count - written);

			if (result <= 0)
				return -1;
			written += result;
		}
	}
	return count < 0 ? -1 : 0;
}

static int write_high_exec_fixture(const char *path)
{
	Elf64_Phdr programs[EXEC_PHDR_MAX];
	Elf64_Ehdr header;
	uint64_t delta, map_end = 0;
	int input = -1, output = -1, result = -1;
	unsigned int index;

	input = open("/bin/exec-runtime", O_RDONLY);
	output = open(path, O_CREAT | O_TRUNC | O_RDWR, 0700);
	if (input < 0 || output < 0 || copy_file_contents(input, output) < 0 ||
	    pread(input, &header, sizeof(header), 0) != sizeof(header) ||
	    memcmp(header.e_ident, ELFMAG, SELFMAG) ||
	    header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_type != ET_EXEC ||
	    header.e_machine != EM_RISCV ||
	    header.e_phentsize != sizeof(programs[0]) ||
	    !header.e_phnum || header.e_phnum > EXEC_PHDR_MAX ||
	    pread(input, programs, header.e_phnum * sizeof(programs[0]),
		  header.e_phoff) !=
		    (ssize_t)(header.e_phnum * sizeof(programs[0])))
		goto out;
	for (index = 0; index < header.e_phnum; index++) {
		uint64_t end;

		if (programs[index].p_type != PT_LOAD ||
		    !programs[index].p_memsz)
			continue;
		if (programs[index].p_vaddr >
		    UINT64_MAX - programs[index].p_memsz)
			goto out;
		end = programs[index].p_vaddr + programs[index].p_memsz;
		if (end > UINT64_MAX - 4095)
			goto out;
		end = (end + 4095) & ~4095ULL;
		if (end > map_end)
			map_end = end;
	}
	if (!map_end || map_end >= EXEC_BRK_LIMIT)
		goto out;
	delta = EXEC_BRK_LIMIT - map_end;
	if (header.e_entry > UINT64_MAX - delta)
		goto out;
	header.e_entry += delta;
	for (index = 0; index < header.e_phnum; index++) {
		if (programs[index].p_type == PT_LOAD &&
		    programs[index].p_align > 1 &&
		    delta % programs[index].p_align)
			goto out;
		if (programs[index].p_vaddr) {
			if (programs[index].p_vaddr > UINT64_MAX - delta)
				goto out;
			programs[index].p_vaddr += delta;
		}
		if (programs[index].p_paddr) {
			if (programs[index].p_paddr > UINT64_MAX - delta)
				goto out;
			programs[index].p_paddr += delta;
		}
	}
	if (pwrite(output, &header, sizeof(header), 0) != sizeof(header) ||
	    pwrite(output, programs, header.e_phnum * sizeof(programs[0]),
		   header.e_phoff) !=
		    (ssize_t)(header.e_phnum * sizeof(programs[0])))
		goto out;
	result = 0;
out:
	if (input >= 0)
		close(input);
	if (output >= 0 && close(output))
		result = -1;
	if (result)
		unlink(path);
	return result;
}

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
	char *invalid_brk[] = { "/tmp/exec-invalid-brk", NULL };
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
	if (write_high_exec_fixture(invalid_brk[0]) < 0)
		return 1;
	if (expect_errno(invalid_brk[0], invalid_brk, ENOMEM) < 0) {
		unlink(invalid_brk[0]);
		return 1;
	}
	if (unlink(invalid_brk[0]))
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
