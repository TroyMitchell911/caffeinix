#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *operation)
{
	printf("SYSTEM_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	exit(1);
}

static void check_identity(void)
{
	struct utsname name;

	if (uname(&name))
		fail("uname");
	if (strcmp(name.sysname, "Caffeinix") ||
	    strcmp(name.nodename, "caffeinix") ||
	    strcmp(name.release, "0.1.0") ||
	    strcmp(name.machine, "riscv64"))
		fail("uname values");
}

static void check_affinity(int expected)
{
	cpu_set_t mask;

	CPU_ZERO(&mask);
	if (sched_getaffinity(0, sizeof(mask), &mask))
		fail("sched_getaffinity");
	if (CPU_COUNT(&mask) != expected)
		fail("affinity count");
	for (int cpu = 0; cpu < expected; cpu++) {
		if (!CPU_ISSET(cpu, &mask))
			fail("affinity bit");
	}
	errno = 0;
	if (sched_getaffinity(0x7fffffff, sizeof(mask), &mask) != -1 ||
	    errno != ESRCH)
		fail("affinity missing pid");
}

struct thread_accounting_state {
	atomic_int ready;
	atomic_int release;
	pid_t tid;
};

static void *thread_accounting_worker(void *argument)
{
	struct thread_accounting_state *state = argument;

	state->tid = syscall(SYS_gettid);
	atomic_store_explicit(&state->ready, 1, memory_order_release);
	while (!atomic_load_explicit(&state->release, memory_order_acquire))
		usleep(1000);
	return NULL;
}

static void check_thread_accounting(int expected_cpus)
{
	struct thread_accounting_state state;
	struct sysinfo before, during, after;
	cpu_set_t mask;
	pthread_t thread;

	atomic_init(&state.ready, 0);
	atomic_init(&state.release, 0);
	state.tid = -1;
	if (sysinfo(&before) ||
	    pthread_create(&thread, NULL, thread_accounting_worker, &state))
		fail("thread accounting setup");
	while (!atomic_load_explicit(&state.ready, memory_order_acquire))
		usleep(1000);
	CPU_ZERO(&mask);
	if (state.tid <= 0 ||
	    sched_getaffinity(state.tid, sizeof(mask), &mask) ||
	    CPU_COUNT(&mask) != expected_cpus)
		fail("thread affinity");
	if (sysinfo(&during) || during.procs != before.procs + 1)
		fail("sysinfo thread count");
	atomic_store_explicit(&state.release, 1, memory_order_release);
	if (pthread_join(thread, NULL) || sysinfo(&after) ||
	    after.procs != before.procs)
		fail("sysinfo thread reap");
}

static void check_sysinfo(void)
{
	struct sysinfo before, during, after;
	pid_t child;
	int status;

	if (sysinfo(&before))
		fail("sysinfo");
	if (!before.totalram || !before.freeram ||
	    before.freeram > before.totalram || before.mem_unit != 1 ||
	    !before.procs)
		fail("sysinfo values");
	child = fork();
	if (child < 0)
		fail("fork");
	if (!child) {
		struct timespec pause = { .tv_nsec = 300000000 };

		(void)nanosleep(&pause, 0);
		_exit(0);
	}
	if (sysinfo(&during) || during.procs <= before.procs)
		fail("sysinfo process count");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		fail("waitpid");
	if (sysinfo(&after) || after.procs != before.procs)
		fail("sysinfo reap count");
}

static pid_t spawn_wait_output_child(int status)
{
	pid_t child = fork();

	if (child < 0)
		fail("wait output fork");
	if (!child)
		_exit(status);
	return child;
}

static void check_wait_lazy_status(void)
{
	int *status;
	pid_t child;

	status = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (status == MAP_FAILED)
		fail("lazy wait status mmap");
	child = spawn_wait_output_child(7);
	if (waitpid(child, status, 0) != child ||
	    !WIFEXITED(*status) || WEXITSTATUS(*status) != 7 ||
	    munmap(status, 4096))
		fail("lazy wait status");
}

static void check_wait_usage(void)
{
	struct rusage usage;
	struct timespec start, now;
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		fail("usage fork");
	if (!child) {
		if (clock_gettime(CLOCK_MONOTONIC, &start))
			_exit(1);
		do {
			if (clock_gettime(CLOCK_MONOTONIC, &now))
				_exit(1);
		} while ((now.tv_sec - start.tv_sec) * 1000000000LL +
			 now.tv_nsec - start.tv_nsec < 20000000);
		_exit(0);
	}
	memset(&usage, 0xa5, sizeof(usage));
	if (wait4(child, &status, 0, &usage) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) ||
	    usage.ru_utime.tv_sec < 0 || usage.ru_utime.tv_usec < 0 ||
	    usage.ru_utime.tv_usec >= 1000000 ||
	    (!usage.ru_utime.tv_sec && !usage.ru_utime.tv_usec) ||
	    usage.ru_stime.tv_sec < 0 || usage.ru_stime.tv_usec < 0 ||
	    usage.ru_stime.tv_usec >= 1000000 ||
	    (!usage.ru_stime.tv_sec && !usage.ru_stime.tv_usec) ||
	    usage.ru_maxrss || usage.ru_nivcsw)
		fail("wait4 usage");
}

static void check_wait_lazy_rusage(void)
{
	struct rusage *usage;
	pid_t child;

	usage = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (usage == MAP_FAILED)
		fail("lazy wait rusage mmap");
	child = spawn_wait_output_child(0);
	if (wait4(child, NULL, 0, usage) != child ||
	    usage->ru_utime.tv_sec < 0 || usage->ru_utime.tv_usec < 0 ||
	    usage->ru_stime.tv_sec < 0 || usage->ru_stime.tv_usec < 0 ||
	    munmap(usage, 4096))
		fail("lazy wait rusage");

	usage = mmap(NULL, 4096, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (usage == MAP_FAILED)
		fail("faulting wait rusage mmap");
	child = spawn_wait_output_child(0);
	errno = 0;
	if (wait4(child, NULL, 0, usage) != -1 || errno != EFAULT ||
	    mprotect(usage, 4096, PROT_READ | PROT_WRITE) ||
	    wait4(child, NULL, 0, usage) != child || munmap(usage, 4096))
		fail("faulting wait rusage");
}

static long long usage_microseconds(const struct rusage *usage)
{
	return ((long long)usage->ru_utime.tv_sec +
		usage->ru_stime.tv_sec) * 1000000LL +
		usage->ru_utime.tv_usec + usage->ru_stime.tv_usec;
}

static void check_wait_descendant_usage(void)
{
	struct rusage child_usage, grandchild_usage;
	int descriptors[2], status;
	pid_t child;

	if (pipe(descriptors))
		fail("descendant usage pipe");
	child = fork();
	if (child < 0)
		fail("descendant usage fork");
	if (!child) {
		struct timespec start, now;
		pid_t grandchild;

		close(descriptors[0]);
		grandchild = fork();
		if (grandchild < 0)
			_exit(1);
		if (!grandchild) {
			if (clock_gettime(CLOCK_MONOTONIC, &start))
				_exit(1);
			do {
				if (clock_gettime(CLOCK_MONOTONIC, &now))
					_exit(1);
			} while ((now.tv_sec - start.tv_sec) * 1000000000LL +
				 now.tv_nsec - start.tv_nsec < 30000000);
			_exit(0);
		}
		memset(&grandchild_usage, 0, sizeof(grandchild_usage));
		if (wait4(grandchild, &status, 0, &grandchild_usage) !=
				grandchild || !WIFEXITED(status) ||
		    WEXITSTATUS(status) ||
		    write(descriptors[1], &grandchild_usage,
			  sizeof(grandchild_usage)) != sizeof(grandchild_usage))
			_exit(1);
		close(descriptors[1]);
		_exit(0);
	}
	close(descriptors[1]);
	if (read(descriptors[0], &grandchild_usage,
		 sizeof(grandchild_usage)) != sizeof(grandchild_usage))
		fail("descendant usage read");
	close(descriptors[0]);
	memset(&child_usage, 0, sizeof(child_usage));
	if (wait4(child, &status, 0, &child_usage) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) ||
	    usage_microseconds(&grandchild_usage) <= 0 ||
	    usage_microseconds(&child_usage) <
		usage_microseconds(&grandchild_usage))
		fail("wait4 descendant usage");
}

int main(int argc, char **argv)
{
	char *end;
	long expected;

	if (argc != 2)
		fail("arguments");
	errno = 0;
	expected = strtol(argv[1], &end, 10);
	if (errno || *end || expected <= 0 || expected > CPU_SETSIZE)
		fail("CPU count argument");
	check_identity();
	check_affinity(expected);
	check_thread_accounting(expected);
	check_sysinfo();
	check_wait_lazy_status();
	check_wait_usage();
	check_wait_lazy_rusage();
	check_wait_descendant_usage();
	puts("SYSTEM_RUNTIME_OK");
	return 0;
}
