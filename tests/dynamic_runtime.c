#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int dynamic_fixture_constructor_value(void);
extern int dynamic_fixture_tls_value(void);
extern int dynamic_fixture_value(void);

static int constructor_value;
static int relro_target = 1;
static int *volatile relro_pointer
	__attribute__((section(".data.rel.ro"))) = &relro_target;
static __thread int runtime_tls
	__attribute__((tls_model("initial-exec"))) = 23;

static void __attribute__((constructor)) runtime_constructor(void)
{
	constructor_value = 11;
}

static void __attribute__((destructor)) runtime_destructor(void)
{
	static const char message[] = "DYNAMIC_DESTRUCTOR_OK\n";

	if (write(STDOUT_FILENO, message, sizeof(message) - 1) < 0)
		_exit(1);
}

static void fail(const char *operation)
{
	printf("DYNAMIC_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	exit(1);
}

static void test_needed_library(void)
{
	if (dynamic_fixture_value() != 17 ||
	    dynamic_fixture_constructor_value() != 13)
		fail("needed library");
	puts("DYNAMIC_NEEDED_OK");
}

static void test_tls(void)
{
	if (runtime_tls != 23 || dynamic_fixture_tls_value() != 29)
		fail("initial TLS");
	runtime_tls = 37;
	if (runtime_tls != 37)
		fail("writable TLS");
	puts("DYNAMIC_TLS_OK");
}

static void test_dlopen(void)
{
	int (*value)(void);
	void *handle;

	handle = dlopen("/lib/libdynamic-dlopen.so", RTLD_NOW | RTLD_LOCAL);
	if (!handle)
		fail("dlopen");
	value = (int (*)(void))dlsym(handle, "dynamic_dlopen_value");
	if (!value || value() != 31)
		fail("dlsym");
	if (dlclose(handle))
		fail("dlclose");
	puts("DYNAMIC_DLOPEN_OK");
}

static void test_pread(void)
{
	char magic[4];
	int fd;

	fd = open("/bin/dynamic-runtime", O_RDONLY);
	if (fd < 0 || pread(fd, magic, sizeof(magic), 0) != sizeof(magic) ||
	    memcmp(magic, "\177ELF", sizeof(magic)) || lseek(fd, 0, SEEK_CUR))
		fail("pread64");
	close(fd);
	puts("DYNAMIC_PREAD_OK");
}

static void test_relro(void)
{
	int status;
	pid_t pid;

	if (relro_pointer != &relro_target || *relro_pointer != 1)
		fail("RELRO relocation");
	pid = fork();
	if (pid < 0)
		fail("RELRO fork");
	if (!pid) {
		relro_pointer = NULL;
		_exit(0);
	}
	if (waitpid(pid, &status, 0) != pid ||
	    !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
		fail("RELRO protection");
	puts("DYNAMIC_RELRO_OK");
}

static void test_exec_cloexec(void)
{
	char descriptor[16];
	int fd, status;
	pid_t pid;

	fd = open("/bin/dynamic-runtime", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail("close-on-exec open");
	if (snprintf(descriptor, sizeof(descriptor), "%d", fd) <= 0)
		fail("close-on-exec descriptor");
	pid = fork();
	if (pid < 0)
		fail("exec fork");
	if (!pid) {
		execl("/bin/dynamic-child", "dynamic-child", descriptor,
		      (char *)NULL);
		_exit(1);
	}
	if (waitpid(pid, &status, 0) != pid ||
	    !WIFEXITED(status) || WEXITSTATUS(status))
		fail("dynamic exec");
	close(fd);
}

static void test_exec_pressure(void)
{
	pid_t children[8];
	int status;

	for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); i++) {
		children[i] = fork();
		if (children[i] < 0)
			fail("pressure fork");
		if (!children[i]) {
			execl("/bin/dynamic-child", "dynamic-child", "pressure",
			      (char *)NULL);
			_exit(1);
		}
	}
	for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); i++) {
		pid_t waited = waitpid(children[i], &status, 0);

		if (waited != children[i] || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			printf("DYNAMIC_PRESSURE_CHILD index=%zu pid=%d "
			       "waited=%d status=%#x\n", i, children[i],
			       waited, status);
			fail("pressure wait");
		}
	}
}

static unsigned int pressure_iterations(const char *argument)
{
	char *end;
	unsigned long iterations;

	errno = 0;
	iterations = strtoul(argument, &end, 10);
	if (errno || !argument[0] || *end || !iterations ||
	    iterations > 10000)
		fail("pressure iterations");
	return iterations;
}

int main(int argc, char **argv)
{
	if ((argc == 2 || argc == 3) && !strcmp(argv[1], "pressure")) {
		unsigned int iterations = argc == 3 ?
			pressure_iterations(argv[2]) : 1;

		while (iterations--)
			test_exec_pressure();
		puts("DYNAMIC_EXEC_PRESSURE_OK");
		return 0;
	}
	if (constructor_value != 11)
		fail("constructor");
	puts("DYNAMIC_CONSTRUCTOR_OK");
	test_needed_library();
	test_tls();
	test_dlopen();
	test_pread();
	test_relro();
	test_exec_cloexec();
	test_exec_pressure();
	puts("DYNAMIC_EXEC_PRESSURE_OK");
	puts("DYNAMIC_RUNTIME_OK");
	return 0;
}
