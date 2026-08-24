#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 16
#define STACK_SIZE (64 * 1024)
#define PAGE_SIZE 4096

struct worker {
	volatile int child_tid;
	volatile int parent_tid;
	volatile int observed_tid;
	volatile int observed_pid;
};

static unsigned char stacks[THREADS][STACK_SIZE]
	__attribute__((aligned(16)));
static unsigned char fault_stack[STACK_SIZE] __attribute__((aligned(16)));
static struct worker workers[THREADS];
static volatile int gate;
static volatile int fault_clone_done;
static volatile int started;
static volatile int vfork_shared;

extern int test_clone(int (*function)(void *), void *stack, int flags,
		      void *argument, int *parent_tid, void *tls,
		      int *child_tid);

static long raw_syscall0(long number)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0");

	__asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
	return a0;
}

static __attribute__((noinline)) long raw_syscall2(
	long number, long argument0, long argument1)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = argument0;
	register long a1 __asm__("a1") = argument1;

	__asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
	return a0;
}

static __attribute__((noinline)) long raw_syscall6(
	long number, long argument0, long argument1, long argument2,
	long argument3, long argument4, long argument5)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = argument0;
	register long a1 __asm__("a1") = argument1;
	register long a2 __asm__("a2") = argument2;
	register long a3 __asm__("a3") = argument3;
	register long a4 __asm__("a4") = argument4;
	register long a5 __asm__("a5") = argument5;

	__asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
			 "r"(a4), "r"(a5), "r"(a7) : "memory");
	return a0;
}

static int worker_main(void *argument)
{
	struct worker *worker = argument;

	worker->observed_tid = raw_syscall0(SYS_gettid);
	worker->observed_pid = raw_syscall0(SYS_getpid);
	__atomic_add_fetch(&started, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&gate, __ATOMIC_ACQUIRE))
		;
	return 0;
}

static int fault_worker_main(void *argument)
{
	(void)argument;
	__atomic_store_n(&fault_clone_done, 1, __ATOMIC_RELEASE);
	return 0;
}

static int fail(const char *reason)
{
	printf("THREAD_RUNTIME_FAIL %s\n", reason);
	return 1;
}

static int test_vfork(void)
{
	long mapping;
	int status;
	pid_t child;

	vfork_shared = 0;
	child = vfork();
	if (child < 0)
		return fail("vfork");
	if (!child) {
		mapping = raw_syscall6(SYS_mmap, 0, 4096,
				       PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping < 0)
			_exit(20);
		*(volatile unsigned char *)mapping = 0x43;
		if (raw_syscall2(SYS_munmap, mapping, 4096))
			_exit(21);
		vfork_shared = 0x43414646;
		_exit(19);
	}
	if (vfork_shared != 0x43414646)
		return fail("vfork shared memory");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 19)
		return fail("vfork wait");
	return 0;
}

static int test_vfork_fatal_signal(int exec_child)
{
	int ready[2], release[2], done[2];
	char done_fd[16];
	char byte = 'x';
	int attempt, status;
	pid_t result, victim;

	if (pipe(ready) || pipe(release) || pipe(done))
		return fail("vfork signal pipes");
	victim = fork();
	if (victim < 0)
		return fail("vfork signal fork");
	if (!victim) {
		pid_t child;

		close(ready[0]);
		close(release[1]);
		close(done[0]);
		snprintf(done_fd, sizeof(done_fd), "%d", done[1]);
		child = vfork();
		if (child < 0)
			_exit(30);
		if (!child) {
			if (write(ready[1], &byte, 1) != 1 ||
			    read(release[0], &byte, 1) != 1)
				_exit(31);
			if (exec_child) {
				execl("/bin/thread-runtime", "thread-runtime",
				      "--vfork-done", done_fd, (char *)0);
				_exit(32);
			}
			if (write(done[1], &byte, 1) != 1)
				_exit(33);
			_exit(0);
		}
		_exit(34);
	}
	close(ready[1]);
	close(release[0]);
	close(done[1]);
	if (read(ready[0], &byte, 1) != 1 || kill(victim, SIGKILL))
		return fail("vfork signal setup");
	result = 0;
	for (attempt = 0; attempt < 1000; attempt++) {
		result = waitpid(victim, &status, WNOHANG);
		if (result == victim)
			break;
		if (result < 0 && errno != EINTR)
			break;
		usleep(1000);
	}
	if (result != victim) {
		write(release[1], &byte, 1);
		while (waitpid(victim, &status, 0) < 0 && errno == EINTR)
			;
		return fail("vfork fatal wait");
	}
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL ||
	    write(release[1], &byte, 1) != 1 ||
	    read(done[0], &byte, 1) != 1)
		return fail("vfork fatal result");
	close(ready[0]);
	close(release[1]);
	close(done[0]);
	return 0;
}

int main(int argc, char **argv)
{
	const int flags = CLONE_VM | CLONE_FS | CLONE_FILES |
		CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
		CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
		CLONE_CHILD_CLEARTID;
	int leader_pid = getpid();
	int leader_tid = syscall(SYS_gettid);
	int *fault_ids;
	unsigned long spins;
	int i, j, tid;

	if (argc == 3 && !strcmp(argv[1], "--vfork-done")) {
		char byte = 'x';
		int descriptor;

		if (sscanf(argv[2], "%d", &descriptor) != 1)
			return 1;
		return write(descriptor, &byte, 1) == 1 ? 0 : 1;
	}

	if (leader_pid != leader_tid)
		return fail("leader pid/tid");
	if (test_vfork())
		return 1;
	if (test_vfork_fatal_signal(0) || test_vfork_fatal_signal(1))
		return 1;
	fault_ids = mmap(0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fault_ids == MAP_FAILED)
		return fail("clone fault mmap");
	tid = test_clone(fault_worker_main, fault_stack + STACK_SIZE,
			 flags & ~CLONE_CHILD_CLEARTID, 0, fault_ids, 0,
			 fault_ids + PAGE_SIZE / sizeof(*fault_ids));
	if (tid <= 0 || fault_ids[0] != tid ||
	    fault_ids[PAGE_SIZE / sizeof(*fault_ids)] != tid)
		return fail("clone fault tid");
	for (spins = 0; spins < 100000000UL; spins++) {
		if (__atomic_load_n(&fault_clone_done, __ATOMIC_ACQUIRE))
			break;
	}
	if (!fault_clone_done || munmap(fault_ids, 2 * PAGE_SIZE) < 0)
		return fail("clone fault completion");
	for (i = 0; i < THREADS; i++) {
		workers[i].child_tid = -1;
		workers[i].parent_tid = -1;
		tid = test_clone(worker_main, stacks[i] + STACK_SIZE, flags,
		                 &workers[i],
		                 (int *)&workers[i].parent_tid,
		                 (void *)0,
		                 (int *)&workers[i].child_tid);
		if (tid <= 0 || workers[i].parent_tid != tid ||
		    workers[i].child_tid != tid)
			return fail("clone tid");
	}
	for (spins = 0; spins < 100000000UL; spins++) {
		if (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == THREADS)
			break;
	}
	if (started != THREADS)
		return fail("start timeout");
	__atomic_store_n(&gate, 1, __ATOMIC_RELEASE);
	for (spins = 0; spins < 100000000UL; spins++) {
		int complete = 1;

		for (i = 0; i < THREADS; i++) {
			if (__atomic_load_n(&workers[i].child_tid,
			                    __ATOMIC_ACQUIRE) != 0) {
				complete = 0;
				break;
			}
		}
		if (complete)
			break;
	}
	for (i = 0; i < THREADS; i++) {
		if (workers[i].child_tid != 0 ||
		    workers[i].observed_pid != leader_pid ||
		    workers[i].observed_tid != workers[i].parent_tid)
			return fail("thread identity");
		for (j = 0; j < i; j++) {
			if (workers[i].observed_tid == workers[j].observed_tid)
				return fail("duplicate tid");
		}
	}
	puts("THREAD_CLONE_OK");
	return 0;
}
