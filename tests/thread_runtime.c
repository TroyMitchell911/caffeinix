#define _GNU_SOURCE

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
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

int main(void)
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

	if (leader_pid != leader_tid)
		return fail("leader pid/tid");
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
