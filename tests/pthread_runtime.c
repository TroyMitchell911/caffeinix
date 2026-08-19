#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 16
#define FD_WORKERS 8
#define CONDITION_WAITERS 8
#define ITERATIONS 1000
#define FD_RACE_ROUNDS 32
#define COPY_PAGE_SIZE 4096UL
#define BREAK_FORK_ROUNDS 128
#define BREAK_GROWTH (4 * COPY_PAGE_SIZE)

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t worker_barrier;
static int counter;
static _Thread_local int tls_value;

static pthread_mutex_t condition_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int condition_ready;
static int condition_go;
static int condition_done;

static pthread_mutex_t robust_lock;
static volatile int detached_done;

static pthread_barrier_t fd_barrier;
static int fd_results[FD_WORKERS];
static pthread_barrier_t fault_barrier;
static unsigned char *fault_mapping;
static volatile int copy_stop;
static volatile int copy_failure;
static unsigned char *copy_mapping;
static int copy_fd;
static volatile int nice_ready;
static volatile int nice_done;
static pid_t nice_tid;
static volatile int break_go;
static volatile int break_stop;
static volatile int break_failure;
static uintptr_t break_base;
static uintptr_t break_target;

static int fail(const char *reason, int error)
{
	printf("PTHREAD_RUNTIME_FAIL %s error=%d\n", reason, error);
	return 1;
}

static void *counter_worker(void *argument)
{
	intptr_t index = (intptr_t)argument;
	int i;

	tls_value = 0x1000 + index;
	for (i = 0; i < ITERATIONS; i++) {
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
	}
	pthread_barrier_wait(&worker_barrier);
	return (void *)(uintptr_t)(tls_value == 0x1000 + index);
}

static void *condition_worker(void *argument)
{
	(void)argument;
	pthread_mutex_lock(&condition_lock);
	condition_ready++;
	while (!condition_go)
		pthread_cond_wait(&condition, &condition_lock);
	condition_done++;
	pthread_mutex_unlock(&condition_lock);
	return 0;
}

static void *robust_owner(void *argument)
{
	(void)argument;
	pthread_mutex_lock(&robust_lock);
	return 0;
}

static void *detached_worker(void *argument)
{
	(void)argument;
	__atomic_store_n(&detached_done, 1, __ATOMIC_RELEASE);
	return 0;
}

static void *fd_worker(void *argument)
{
	intptr_t index = (intptr_t)argument;

	pthread_barrier_wait(&fd_barrier);
	fd_results[index] = open("/dev/null", O_RDWR);
	return 0;
}

static int test_shared_fd_table(void)
{
	pthread_t threads[WORKERS];
	int error, first, second, round;

	for (round = 0; round < FD_RACE_ROUNDS; round++) {
		if (pthread_barrier_init(&fd_barrier, 0, FD_WORKERS + 1))
			return fail("fd barrier init", round);
		for (first = 0; first < FD_WORKERS; first++) {
			fd_results[first] = -1;
			error = pthread_create(&threads[first], 0, fd_worker,
			                       (void *)(intptr_t)first);
			if (error)
				return fail("fd create", error);
		}
		pthread_barrier_wait(&fd_barrier);
		for (first = 0; first < FD_WORKERS; first++) {
			error = pthread_join(threads[first], 0);
			if (error || fd_results[first] < 0)
				return fail("fd join", error);
			for (second = 0; second < first; second++)
				if (fd_results[first] == fd_results[second])
					return fail("duplicate fd",
					            fd_results[first]);
		}
		for (first = 0; first < FD_WORKERS; first++)
			if (close(fd_results[first]))
				return fail("fd close", errno);
		pthread_barrier_destroy(&fd_barrier);
	}
	return 0;
}

static void *break_worker(void *argument)
{
	long result;

	(void)argument;
	while (!__atomic_load_n(&break_go, __ATOMIC_ACQUIRE))
		;
	while (!__atomic_load_n(&break_stop, __ATOMIC_ACQUIRE)) {
		result = syscall(SYS_brk, break_target);
		if ((uintptr_t)result != break_target) {
			__atomic_store_n(&break_failure, 1, __ATOMIC_RELEASE);
			break;
		}
		*(volatile unsigned char *)(break_target - 1) = 0xa5;
		result = syscall(SYS_brk, break_base);
		if ((uintptr_t)result != break_base) {
			__atomic_store_n(&break_failure, 2, __ATOMIC_RELEASE);
			break;
		}
	}
	return 0;
}

static int concurrent_break_fork_child(void)
{
	pthread_t thread;
	volatile unsigned char value;
	uintptr_t current;
	pid_t child;
	int error, round, status;

	break_go = 0;
	break_stop = 0;
	break_failure = 0;
	error = pthread_create(&thread, 0, break_worker, 0);
	if (error)
		return 10;
	break_base = syscall(SYS_brk, 0);
	break_target = break_base + BREAK_GROWTH;
	if (break_target < break_base)
		return 11;
	__atomic_store_n(&break_go, 1, __ATOMIC_RELEASE);
	for (round = 0; round < BREAK_FORK_ROUNDS; round++) {
		if (__atomic_load_n(&break_failure, __ATOMIC_ACQUIRE))
			return 12;
		child = fork();
		if (child < 0)
			return 13;
		if (!child) {
			current = syscall(SYS_brk, 0);
			if (current > break_base) {
				value = *(volatile unsigned char *)(current - 1);
				(void)value;
			}
			_exit(EXIT_SUCCESS);
		}
		if (waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
			return 14;
	}
	__atomic_store_n(&break_stop, 1, __ATOMIC_RELEASE);
	error = pthread_join(thread, 0);
	if ((uintptr_t)syscall(SYS_brk, break_base) != break_base)
		return 15;
	if (error || break_failure)
		return 16;
	return 0;
}

static int test_concurrent_break_fork(void)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return fail("break helper fork", errno);
	if (!child)
		_exit(concurrent_break_fork_child());
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		return fail("break fork snapshot", status);
	return 0;
}

static void *fault_worker(void *argument)
{
	volatile unsigned char value;

	(void)argument;
	pthread_barrier_wait(&fault_barrier);
	value = fault_mapping[0];
	return (void *)(uintptr_t)value;
}

static int test_concurrent_fault(void)
{
	pthread_t threads[WORKERS];
	void *result;
	int error, index;

	fault_mapping = mmap(0, COW_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fault_mapping == MAP_FAILED)
		return fail("fault mmap", errno);
	if (pthread_barrier_init(&fault_barrier, 0, WORKERS + 1))
		return fail("fault barrier init", errno);
	for (index = 0; index < WORKERS; index++) {
		error = pthread_create(&threads[index], 0, fault_worker, 0);
		if (error)
			return fail("fault create", error);
	}
	pthread_barrier_wait(&fault_barrier);
	for (index = 0; index < WORKERS; index++) {
		error = pthread_join(threads[index], &result);
		if (error || result)
			return fail("fault join", error);
	}
	pthread_barrier_destroy(&fault_barrier);
	if (munmap(fault_mapping, COW_PAGE_SIZE))
		return fail("fault munmap", errno);
	return 0;
}

static void *copy_worker(void *argument)
{
	ssize_t result;

	(void)argument;
	while (!__atomic_load_n(&copy_stop, __ATOMIC_ACQUIRE)) {
		errno = 0;
		result = read(copy_fd, copy_mapping, COPY_PAGE_SIZE);
		if (result != (ssize_t)COPY_PAGE_SIZE &&
		    !(result == -1 && errno == EFAULT)) {
			__atomic_store_n(&copy_failure, errno ? errno : 1,
			                 __ATOMIC_RELEASE);
			break;
		}
	}
	return 0;
}

static int test_copy_unmap_race(void)
{
	pthread_t thread;
	void *mapped;
	int error, iteration;

	copy_mapping = mmap(0, COPY_PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (copy_mapping == MAP_FAILED)
		return fail("copy mmap", errno);
	copy_fd = open("/dev/zero", O_RDONLY);
	if (copy_fd < 0)
		return fail("copy open", errno);
	copy_stop = 0;
	copy_failure = 0;
	error = pthread_create(&thread, 0, copy_worker, 0);
	if (error)
		return fail("copy create", error);
	for (iteration = 0; iteration < ITERATIONS; iteration++) {
		if (munmap(copy_mapping, COPY_PAGE_SIZE))
			return fail("copy race munmap", errno);
		mapped = mmap(copy_mapping, COPY_PAGE_SIZE,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (mapped != copy_mapping)
			return fail("copy race mmap", errno);
	}
	__atomic_store_n(&copy_stop, 1, __ATOMIC_RELEASE);
	error = pthread_join(thread, 0);
	if (error || copy_failure)
		return fail("copy race join", error ? error : copy_failure);
	if (close(copy_fd) || munmap(copy_mapping, COPY_PAGE_SIZE))
		return fail("copy race cleanup", errno);
	return 0;
}

static void *nice_worker(void *argument)
{
	(void)argument;
	nice_tid = syscall(SYS_gettid);
	if (setpriority(PRIO_PROCESS, 0, 7) ||
	    getpriority(PRIO_PROCESS, 0) != 7) {
		__atomic_store_n(&nice_ready, -1, __ATOMIC_RELEASE);
		return 0;
	}
	__atomic_store_n(&nice_ready, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&nice_done, __ATOMIC_ACQUIRE))
		;
	return 0;
}

static int test_thread_nice(void)
{
	pthread_t thread;
	int error;

	nice_ready = 0;
	nice_done = 0;
	error = pthread_create(&thread, 0, nice_worker, 0);
	if (error)
		return fail("nice create", error);
	while (!__atomic_load_n(&nice_ready, __ATOMIC_ACQUIRE))
		;
	if (nice_ready < 0 || getpriority(PRIO_PROCESS, nice_tid) != 7 ||
	    getpriority(PRIO_PROCESS, 0) != 0)
		return fail("thread nice", errno);
	__atomic_store_n(&nice_done, 1, __ATOMIC_RELEASE);
	if (pthread_join(thread, 0))
		return fail("nice join", errno);
	return 0;
}

int main(void)
{
	pthread_t threads[WORKERS];
	pthread_mutexattr_t mutex_attribute;
	pthread_condattr_t condition_attribute;
	pthread_attr_t thread_attribute;
	pthread_cond_t timed_condition;
	pthread_mutex_t timed_lock = PTHREAD_MUTEX_INITIALIZER;
	struct timespec deadline;
	void *result;
	unsigned long spins;
	int error, i;

	error = pthread_barrier_init(&worker_barrier, 0, WORKERS + 1);
	if (error)
		return fail("barrier init", error);
	for (i = 0; i < WORKERS; i++) {
		error = pthread_create(&threads[i], 0, counter_worker,
		                       (void *)(intptr_t)i);
		if (error)
			return fail("create", error);
	}
	pthread_barrier_wait(&worker_barrier);
	for (i = 0; i < WORKERS; i++) {
		error = pthread_join(threads[i], &result);
		if (error || (uintptr_t)result != 1)
			return fail("join or TLS", error);
	}
	if (counter != WORKERS * ITERATIONS)
		return fail("mutex counter", counter);
	pthread_barrier_destroy(&worker_barrier);

	for (i = 0; i < CONDITION_WAITERS; i++) {
		error = pthread_create(&threads[i], 0, condition_worker, 0);
		if (error)
			return fail("condition create", error);
	}
	for (spins = 0; spins < 100000000UL; spins++) {
		pthread_mutex_lock(&condition_lock);
		i = condition_ready;
		pthread_mutex_unlock(&condition_lock);
		if (i == CONDITION_WAITERS)
			break;
	}
	if (condition_ready != CONDITION_WAITERS)
		return fail("condition ready", condition_ready);
	pthread_mutex_lock(&condition_lock);
	condition_go = 1;
	pthread_cond_broadcast(&condition);
	pthread_mutex_unlock(&condition_lock);
	for (i = 0; i < CONDITION_WAITERS; i++) {
		error = pthread_join(threads[i], 0);
		if (error)
			return fail("condition join", error);
	}
	if (condition_done != CONDITION_WAITERS)
		return fail("condition broadcast", condition_done);

	pthread_condattr_init(&condition_attribute);
	error = pthread_condattr_setclock(&condition_attribute,
	                                  CLOCK_MONOTONIC);
	if (error)
		return fail("condition clock", error);
	pthread_cond_init(&timed_condition, &condition_attribute);
	pthread_condattr_destroy(&condition_attribute);
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_nsec += 20000000;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&timed_lock);
	error = pthread_cond_timedwait(&timed_condition, &timed_lock,
	                               &deadline);
	pthread_mutex_unlock(&timed_lock);
	if (error != ETIMEDOUT)
		return fail("condition timeout", error);
	pthread_cond_destroy(&timed_condition);
	pthread_mutex_destroy(&timed_lock);

	pthread_mutexattr_init(&mutex_attribute);
	pthread_mutexattr_setpshared(&mutex_attribute,
	                            PTHREAD_PROCESS_SHARED);
	error = pthread_mutexattr_setrobust(&mutex_attribute,
	                                    PTHREAD_MUTEX_ROBUST);
	if (error)
		return fail("robust attribute", error);
	pthread_mutex_init(&robust_lock, &mutex_attribute);
	pthread_mutexattr_destroy(&mutex_attribute);
	error = pthread_create(&threads[0], 0, robust_owner, 0);
	if (error)
		return fail("robust create", error);
	pthread_join(threads[0], 0);
	error = pthread_mutex_lock(&robust_lock);
	if (error != EOWNERDEAD)
		return fail("robust owner death", error);
	pthread_mutex_consistent(&robust_lock);
	pthread_mutex_unlock(&robust_lock);
	pthread_mutex_destroy(&robust_lock);

	pthread_attr_init(&thread_attribute);
	pthread_attr_setdetachstate(&thread_attribute,
	                           PTHREAD_CREATE_DETACHED);
	error = pthread_create(&threads[0], &thread_attribute,
	                       detached_worker, 0);
	pthread_attr_destroy(&thread_attribute);
	if (error)
		return fail("detached create", error);
	for (spins = 0; spins < 100000000UL &&
	     !__atomic_load_n(&detached_done, __ATOMIC_ACQUIRE); spins++)
		;
	if (!detached_done)
		return fail("detached timeout", 0);
	if (test_shared_fd_table() || test_thread_nice())
		return 1;
	if (test_concurrent_break_fork())
		return 1;
	if (test_concurrent_fault())
		return 1;
	if (test_copy_unmap_race())
		return 1;

	puts("PTHREAD_RUNTIME_OK");
	return 0;
}
