#define _GNU_SOURCE

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GPR_BEFORE 0x123UL
#define GPR_AFTER  0x456UL
#define FP_BEFORE  0x3ff8000000000000ULL
#define FP_AFTER   0x4004000000000000ULL
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define EXIT_GROUP_THREADS 8
#define EXIT_GROUP_ROUNDS 4
#define REALTIME_SIGNAL_COUNT 8
#define PROCESS_WAITERS 4

extern int signal_register_probe(uint64_t *gpr, uint64_t *fp);
extern long signal_a0_probe(long pid, long tid, long signal);

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t handler_error;
static volatile sig_atomic_t nested_depth;
static volatile sig_atomic_t child_events;
static volatile sig_atomic_t target_count;
static volatile sig_atomic_t target_tid_seen;
static volatile sig_atomic_t interrupt_count;
static atomic_int target_ready;
static atomic_int target_done;
static atomic_int cancel_ready;
static atomic_int exit_group_ready;
static atomic_int process_wait_ready;
static atomic_int process_wait_done;
static atomic_int process_wait_interrupted;
static atomic_int process_wait_failed;
static volatile int restart_futex;
static volatile int exit_group_futex;
static pid_t expected_sender;
static pid_t target_tid;
static unsigned char alternate_stack[SIGSTKSZ];

enum sender_operation {
	SENDER_SIGNAL_ONLY,
	SENDER_WAKE_FUTEX,
	SENDER_KILL_CHILD,
};

struct sender_request {
	pthread_t target;
	enum sender_operation operation;
	pid_t child;
	int observed_count;
};

static void fail(const char *stage)
{
	printf("SIGNAL_RUNTIME_FAIL %s errno=%d\n", stage, errno);
	exit(1);
}

static void require(int condition, const char *stage)
{
	if (!condition)
		fail(stage);
}

static int wait_for_count(volatile sig_atomic_t *value, int expected)
{
	struct timespec start, now;

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return -1;
	while (*value < expected) {
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			return -1;
		if (now.tv_sec - start.tv_sec >= 2)
			return -1;
	}
	return 0;
}

static int delay_milliseconds(unsigned int milliseconds)
{
	struct timespec start, now;
	uint64_t elapsed;

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return -1;
	for (;;) {
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			return -1;
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000;
		if (now.tv_nsec >= start.tv_nsec)
			elapsed += (now.tv_nsec - start.tv_nsec) / 1000000;
		else
			elapsed -= (start.tv_nsec - now.tv_nsec + 999999) /
			           1000000;
		if (elapsed >= milliseconds)
			return 0;
	}
}

static void interrupt_handler(int signal)
{
	if (signal != SIGUSR1)
		handler_error = 10;
	interrupt_count++;
}

static void *interrupt_sender(void *argument)
{
	struct sender_request *request = argument;
	unsigned long attempts;

	if (delay_milliseconds(10) ||
	    pthread_kill(request->target, SIGUSR1)) {
		handler_error = 11;
		return 0;
	}
	if (request->operation == SENDER_SIGNAL_ONLY)
		return 0;
	while (interrupt_count == request->observed_count)
		;
	if (request->operation == SENDER_KILL_CHILD) {
		if (kill(request->child, SIGTERM))
			handler_error = 12;
		return 0;
	}
	for (attempts = 0; attempts < 1000000UL; attempts++) {
		long result = syscall(SYS_futex, &restart_futex,
		                      FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
		                      1, 0, 0, 0);

		if (result == 1)
			return 0;
		if (result < 0) {
			handler_error = 13;
			return 0;
		}
	}
	handler_error = 14;
	return 0;
}

static void basic_handler(int signal, siginfo_t *information, void *context)
{
	(void)context;
	if (signal != SIGUSR1 || information->si_signo != SIGUSR1 ||
	    information->si_code != SI_USER ||
	    information->si_pid != expected_sender)
		handler_error = 1;
	handler_count++;
}

static void register_handler(int signal, siginfo_t *information,
			     void *context)
{
	ucontext_t *user_context = context;
	unsigned long long *floating;

	(void)information;
	if (signal != SIGUSR2)
		handler_error = 2;
	if (user_context->uc_mcontext.__gregs[REG_S2] != GPR_BEFORE)
		handler_error = 3;
	floating = user_context->uc_mcontext.__fpregs.__d.__f;
	if (floating[8] != FP_BEFORE)
		handler_error = 4;
	user_context->uc_mcontext.__gregs[REG_S2] = GPR_AFTER;
	floating[8] = FP_AFTER;
}

static void restart_marker_handler(int signal, siginfo_t *information,
				   void *context)
{
	ucontext_t *user_context = context;

	(void)information;
	if (signal != SIGUSR2)
		handler_error = 15;
	user_context->uc_mcontext.__gregs[REG_A0] = (greg_t)-512;
}

static void nested_handler(int signal, siginfo_t *information, void *context)
{
	unsigned char stack_byte;
	uintptr_t address = (uintptr_t)&stack_byte;

	(void)information;
	(void)context;
	if (signal != SIGUSR2 || address < (uintptr_t)alternate_stack ||
	    address >= (uintptr_t)alternate_stack + sizeof(alternate_stack))
		handler_error = 5;
	nested_depth++;
	handler_count++;
	if (nested_depth == 1 && raise(SIGUSR2))
		handler_error = 6;
	nested_depth--;
}

static void target_handler(int signal)
{
	if (signal != SIGUSR1)
		handler_error = 7;
	target_tid_seen = syscall(SYS_gettid);
	target_count++;
}

static void child_handler(int signal, siginfo_t *information, void *context)
{
	(void)signal;
	(void)context;
	if (information->si_code == CLD_STOPPED)
		child_events |= 1;
	else if (information->si_code == CLD_CONTINUED)
		child_events |= 2;
	else if (information->si_code == CLD_KILLED)
		child_events |= 4;
	else if (information->si_code == CLD_EXITED)
		child_events |= 8;
}

static void fault_handler(int signal, siginfo_t *information, void *context)
{
	(void)context;
	if (signal != SIGSEGV)
		_exit(110);
	if (information->si_code != SEGV_MAPERR)
		_exit(120 + (information->si_code & 0x7f));
	if (information->si_addr != (void *)0x1000)
		_exit(128 + (((uintptr_t)information->si_addr >> 12) & 0x7f));
	_exit(0);
}

static void reset_handler(int signal)
{
	(void)signal;
}

static void *process_sender(void *argument)
{
	(void)argument;
	if (kill(getpid(), SIGUSR1))
		handler_error = 8;
	return 0;
}

static void *target_thread(void *argument)
{
	sigset_t set;

	(void)argument;
	target_tid = syscall(SYS_gettid);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	if (pthread_sigmask(SIG_UNBLOCK, &set, 0)) {
		handler_error = 9;
		return 0;
	}
	atomic_store_explicit(&target_ready, 1, memory_order_release);
	while (!atomic_load_explicit(&target_done, memory_order_acquire))
		;
	return 0;
}

static void *cancel_thread(void *argument)
{
	char byte;

	(void)argument;
	atomic_store_explicit(&cancel_ready, 1, memory_order_release);
	(void)read(STDIN_FILENO, &byte, sizeof(byte));
	return (void *)1;
}

static void *exit_group_thread(void *argument)
{
	(void)argument;
	atomic_fetch_add_explicit(&exit_group_ready, 1,
	                          memory_order_release);
	(void)syscall(SYS_futex, &exit_group_futex,
	              FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
	return 0;
}

static void *process_wait_thread(void *argument)
{
	struct timespec timeout = { .tv_nsec = 200000000 };
	sigset_t set;
	int result;

	(void)argument;
	sigfillset(&set);
	sigdelset(&set, SIGUSR1);
	atomic_fetch_add_explicit(&process_wait_ready, 1,
	                          memory_order_release);
	errno = 0;
	result = ppoll(0, 0, &timeout, &set);
	if (result == -1 && errno == EINTR)
		atomic_fetch_add(&process_wait_interrupted, 1);
	else if (result)
		atomic_store(&process_wait_failed, 1);
	atomic_fetch_add_explicit(&process_wait_done, 1,
	                          memory_order_release);
	return 0;
}

static void *timed_signal_sender(void *argument)
{
	pthread_t target = *(pthread_t *)argument;
	int index;

	for (index = 0; index < 3; index++) {
		if (delay_milliseconds(50) || pthread_kill(target, SIGUSR1)) {
			handler_error = 16;
			break;
		}
	}
	return 0;
}

static void *exit_group_socket_thread(void *argument)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	char byte;
	int fd;

	(void)argument;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&address,
			  sizeof(address)))
		_exit(106);
	atomic_fetch_add_explicit(&exit_group_ready, 1,
	                          memory_order_release);
	(void)recv(fd, &byte, sizeof(byte), 0);
	close(fd);
	return 0;
}

static void test_basic_delivery(void)
{
	struct sigaction action = { 0 };

	handler_count = 0;
	handler_error = 0;
	expected_sender = getpid();
	action.sa_sigaction = basic_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0), "install basic handler");
	require(!kill(getpid(), SIGUSR1), "send process signal");
	require(handler_count == 1 && !handler_error, "basic delivery");
}

static void test_register_frame(void)
{
	struct sigaction action = { 0 };
	uint64_t gpr = 0, fp = 0;

	action.sa_sigaction = register_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR2, &action, 0), "install frame handler");
	require(!signal_register_probe(&gpr, &fp), "register probe");
	require(!handler_error && gpr == GPR_AFTER && fp == FP_AFTER,
	        "restore register frame");
	action.sa_sigaction = restart_marker_handler;
	require(!sigaction(SIGUSR2, &action, 0),
	        "install restart marker handler");
	require(signal_a0_probe(getpid(), syscall(SYS_gettid), SIGUSR2) ==
	        -512, "preserve sigreturn a0");
	require(!handler_error, "restart marker handler");
}

static void test_process_signal_wake(void)
{
	struct sigaction action = { 0 };
	pthread_t threads[PROCESS_WAITERS];
	sigset_t set, old;
	int index;

	action.sa_handler = target_handler;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0),
	        "install process wake handler");
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	require(!pthread_sigmask(SIG_BLOCK, &set, &old),
	        "block process wake main");
	atomic_store(&process_wait_ready, 0);
	atomic_store(&process_wait_done, 0);
	atomic_store(&process_wait_interrupted, 0);
	atomic_store(&process_wait_failed, 0);
	for (index = 0; index < PROCESS_WAITERS; index++)
		require(!pthread_create(&threads[index], 0,
					process_wait_thread, 0),
		        "create process waiter");
	while (atomic_load_explicit(&process_wait_ready,
	                           memory_order_acquire) != PROCESS_WAITERS)
		;
	require(!kill(getpid(), SIGUSR1), "send process wake signal");
	while (!atomic_load_explicit(&process_wait_done,
	                            memory_order_acquire))
		;
	require(!delay_milliseconds(20), "settle process wake");
	if (atomic_load(&process_wait_done) != 1 ||
	    atomic_load(&process_wait_interrupted) != 1)
		printf("SIGNAL_WAKE_COUNTS done=%d interrupted=%d failed=%d\n",
		       atomic_load(&process_wait_done),
		       atomic_load(&process_wait_interrupted),
		       atomic_load(&process_wait_failed));
	require(atomic_load(&process_wait_done) == 1 &&
	        atomic_load(&process_wait_interrupted) == 1,
	        "wake one process waiter");
	for (index = 0; index < PROCESS_WAITERS; index++)
		require(!pthread_join(threads[index], 0),
		        "join process waiter");
	require(!atomic_load(&process_wait_failed) &&
	        atomic_load(&process_wait_done) == PROCESS_WAITERS &&
	        atomic_load(&process_wait_interrupted) == 1,
	        "process wake results");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore process wake mask");
}

static void test_pending_and_wait(void)
{
	struct timespec timeout = { .tv_nsec = 20000000 };
	siginfo_t information;
	sigset_t set, pending, old;
	int before, result;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	require(!sigprocmask(SIG_BLOCK, &set, &old), "block signal");
	before = handler_count;
	require(!kill(getpid(), SIGUSR1), "queue blocked signal");
	require(!kill(getpid(), SIGUSR1), "coalesce blocked signal");
	require(!sigpending(&pending) && sigismember(&pending, SIGUSR1) == 1,
	        "report pending signal");
	require(handler_count == before, "hold blocked signal");
	require(!sigprocmask(SIG_UNBLOCK, &set, 0), "unblock signal");
	require(handler_count == before + 1, "deliver unblocked signal");
	require(!sigprocmask(SIG_SETMASK, &old, 0), "restore signal mask");

	sigemptyset(&set);
	sigaddset(&set, SIGKILL);
	sigaddset(&set, SIGSTOP);
	require(!sigprocmask(SIG_BLOCK, &set, &old),
	        "attempt to block unmaskable signals");
	require(!sigprocmask(SIG_SETMASK, 0, &pending) &&
	        sigismember(&pending, SIGKILL) == 0 &&
	        sigismember(&pending, SIGSTOP) == 0,
	        "keep kill and stop unblocked");
	require(!sigprocmask(SIG_SETMASK, &old, 0),
	        "restore unmaskable signal test mask");

	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	require(!sigprocmask(SIG_BLOCK, &set, &old), "block waited signal");
	require(!kill(getpid(), SIGUSR2), "queue waited signal");
	result = sigtimedwait(&set, &information, &timeout);
	require(result == SIGUSR2 && information.si_signo == SIGUSR2 &&
	        information.si_code == SI_USER &&
	        information.si_pid == getpid(), "consume waited signal");
	errno = 0;
	result = sigtimedwait(&set, &information, &timeout);
	require(result == -1 && errno == EAGAIN, "time out signal wait");
	require(!sigprocmask(SIG_SETMASK, &old, 0), "restore waited mask");
}

static void test_default_ignored_wait(void)
{
	struct timespec timeout = { .tv_nsec = 200000000 };
	struct sigaction action = { .sa_handler = SIG_DFL };
	siginfo_t information;
	sigset_t set, old;
	pid_t child;
	int status;

	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGCHLD, &action, 0),
	        "restore default child action");
	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigaddset(&set, SIGURG);
	require(!pthread_sigmask(SIG_BLOCK, &set, &old),
	        "block default ignored signals");
	require(!pthread_kill(pthread_self(), SIGURG),
	        "queue ignored thread signal");
	require(sigtimedwait(&set, &information, &timeout) == SIGURG &&
	        information.si_signo == SIGURG,
	        "wait for ignored thread signal");
	child = fork();
	require(child >= 0, "fork ignored wait child");
	if (!child)
		_exit(23);
	require(sigtimedwait(&set, &information, &timeout) == SIGCHLD &&
	        information.si_signo == SIGCHLD &&
	        information.si_code == CLD_EXITED,
	        "wait for default child signal");
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	        WEXITSTATUS(status) == 23, "reap ignored wait child");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore default ignored mask");
}

static void test_pending_priority_and_realtime(void)
{
	struct timespec timeout = { .tv_nsec = 20000000 };
	siginfo_t information;
	sigset_t set, old;
	int index, result;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	require(!pthread_sigmask(SIG_BLOCK, &set, &old),
	        "block priority signals");
	require(!pthread_kill(pthread_self(), SIGUSR2),
	        "queue thread priority signal");
	require(!kill(getpid(), SIGUSR1), "queue process priority signal");
	result = sigtimedwait(&set, &information, &timeout);
	require(result == SIGUSR1 && information.si_signo == SIGUSR1,
	        "select lowest pending signal");
	result = sigtimedwait(&set, &information, &timeout);
	require(result == SIGUSR2 && information.si_signo == SIGUSR2,
	        "consume thread pending signal");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore priority signal mask");

	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN);
	require(!pthread_sigmask(SIG_BLOCK, &set, &old),
	        "block realtime signal");
	for (index = 0; index < REALTIME_SIGNAL_COUNT; index++)
		require(!pthread_kill(pthread_self(), SIGRTMIN),
		        "queue realtime signal");
	for (index = 0; index < REALTIME_SIGNAL_COUNT; index++) {
		result = sigtimedwait(&set, &information, &timeout);
		require(result == SIGRTMIN &&
		        information.si_signo == SIGRTMIN &&
		        information.si_code == SI_TKILL,
		        "consume queued realtime signal");
	}
	errno = 0;
	require(sigtimedwait(&set, &information, &timeout) == -1 &&
	        errno == EAGAIN, "drain realtime signal queue");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore realtime signal mask");
}

static void test_sigsuspend(void)
{
	struct sigaction action = { 0 };
	pthread_t sender;
	sigset_t block, empty, old, current;
	int before;

	action.sa_sigaction = basic_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0), "install suspend handler");
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	require(!pthread_sigmask(SIG_BLOCK, &block, &old), "block suspend signal");
	before = handler_count;
	require(!pthread_create(&sender, 0, process_sender, 0),
	        "create suspend sender");
	sigemptyset(&empty);
	errno = 0;
	require(sigsuspend(&empty) == -1 && errno == EINTR,
	        "interrupt sigsuspend");
	require(!pthread_join(sender, 0), "join suspend sender");
	require(handler_count == before + 1 && !handler_error,
	        "deliver suspend signal");
	require(!pthread_sigmask(SIG_SETMASK, 0, &current) &&
	        sigismember(&current, SIGUSR1) == 1,
	        "restore suspend mask");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore suspend caller mask");
}

static void test_altstack(void)
{
	struct sigaction action = { 0 };
	stack_t requested, current;

	requested.ss_sp = alternate_stack;
	requested.ss_size = sizeof(alternate_stack);
	requested.ss_flags = 0;
	require(!sigaltstack(&requested, 0), "install alternate stack");
	handler_count = 0;
	nested_depth = 0;
	handler_error = 0;
	action.sa_sigaction = nested_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR2, &action, 0), "install nested handler");
	require(!raise(SIGUSR2), "raise nested signal");
	require(handler_count == 2 && !nested_depth && !handler_error,
	        "nested alternate stack");
	require(!sigaltstack(0, &current) && !(current.ss_flags & SS_ONSTACK) &&
	        current.ss_sp == alternate_stack,
	        "query alternate stack");
	requested.ss_flags = SS_DISABLE;
	require(!sigaltstack(&requested, 0), "disable alternate stack");
}

static void test_thread_targeting(void)
{
	struct sigaction action = { 0 };
	pthread_t thread;
	sigset_t set, old;

	handler_error = 0;
	target_count = 0;
	target_tid_seen = 0;
	atomic_store(&target_ready, 0);
	atomic_store(&target_done, 0);
	action.sa_handler = target_handler;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0), "install thread handler");
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	require(!pthread_sigmask(SIG_BLOCK, &set, &old),
	        "block main thread signal");
	require(!pthread_create(&thread, 0, target_thread, 0),
	        "create target thread");
	while (!atomic_load_explicit(&target_ready, memory_order_acquire))
		;
	require(syscall(SYS_tkill, target_tid, SIGUSR1) == 0,
	        "tkill target thread");
	require(!wait_for_count(&target_count, 1), "observe tkill");
	require(syscall(SYS_tgkill, getpid(), target_tid, SIGUSR1) == 0,
	        "tgkill target thread");
	require(!wait_for_count(&target_count, 2), "observe tgkill");
	require(!pthread_kill(thread, SIGUSR1), "pthread_kill target thread");
	require(!wait_for_count(&target_count, 3), "observe pthread_kill");
	atomic_store_explicit(&target_done, 1, memory_order_release);
	require(!pthread_join(thread, 0), "join target thread");
	require(!handler_error && target_tid_seen == target_tid,
	        "thread directed delivery");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore main thread mask");
}

static void test_child_events(void)
{
	struct sigaction action = { 0 };
	pid_t child, result;
	int status;

	child_events = 0;
	action.sa_sigaction = child_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGCHLD, &action, 0), "install child handler");
	child = fork();
	require(child >= 0, "fork stopped child");
	if (!child) {
		for (;;)
			__asm__ volatile("" ::: "memory");
	}
	require(!kill(child, SIGSTOP), "stop child");
	result = waitpid(child, &status, WUNTRACED);
	require(result == child && WIFSTOPPED(status) &&
	        WSTOPSIG(status) == SIGSTOP && (child_events & 1),
	        "wait stopped child");
	require(!kill(child, SIGCONT), "continue child");
	result = waitpid(child, &status, WCONTINUED);
	require(result == child && WIFCONTINUED(status) &&
	        (child_events & 2), "wait continued child");
	require(!kill(child, SIGTERM), "terminate child");
	result = waitpid(child, &status, 0);
	require(result == child && WIFSIGNALED(status) &&
	        WTERMSIG(status) == SIGTERM && (child_events & 4),
	        "wait signaled child");

	child_events = 0;
	action.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
	require(!sigaction(SIGCHLD, &action, 0),
	        "install no-cld-stop handler");
	child = fork();
	require(child >= 0, "fork no-cld-stop child");
	if (!child) {
		for (;;)
			__asm__ volatile("" ::: "memory");
	}
	require(!kill(child, SIGSTOP), "stop no-cld-stop child");
	require(waitpid(child, &status, WUNTRACED) == child &&
	        WIFSTOPPED(status) && child_events == 0,
	        "suppress stopped SIGCHLD");
	require(!kill(child, SIGCONT), "continue no-cld-stop child");
	require(waitpid(child, &status, WCONTINUED) == child &&
	        WIFCONTINUED(status) && child_events == 0,
	        "suppress continued SIGCHLD");
	require(!kill(child, SIGTERM), "terminate no-cld-stop child");
	require(waitpid(child, &status, 0) == child &&
	        WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM &&
	        child_events == 4, "retain exit SIGCHLD");
}

static void test_exec_dispositions(void)
{
	struct sigaction action = { 0 };
	pid_t child;
	int status;

	action.sa_handler = reset_handler;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0),
	        "install inherited exec handler");
	child = fork();
	require(child >= 0, "fork exec-reset child");
	if (!child) {
		execl("/bin/signal-runtime", "signal-runtime",
		      "exec-reset", NULL);
		_exit(106);
	}
	require(waitpid(child, &status, 0) == child &&
	        WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
	        "reset caught disposition on exec");

	action.sa_handler = SIG_IGN;
	require(!sigaction(SIGUSR2, &action, 0),
	        "install inherited ignored action");
	child = fork();
	require(child >= 0, "fork exec-ignore child");
	if (!child) {
		execl("/bin/signal-runtime", "signal-runtime",
		      "exec-ignore", NULL);
		_exit(107);
	}
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	        WEXITSTATUS(status) == 0,
	        "preserve ignored disposition on exec");
	action.sa_handler = SIG_DFL;
	require(!sigaction(SIGUSR2, &action, 0),
	        "restore ignored exec action");
}

static void test_child_auto_reap(void)
{
	struct sigaction action = { 0 };
	pid_t child;
	int round, status;

	action.sa_sigaction = child_handler;
	action.sa_flags = SA_SIGINFO | SA_NOCLDWAIT;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGCHLD, &action, 0),
	        "install no-cld-wait handler");
	for (round = 0; round < 8; round++) {
		child_events = 0;
		child = fork();
		require(child >= 0, "fork no-cld-wait child");
		if (!child)
			_exit(0);
		errno = 0;
		require(waitpid(child, &status, 0) == -1 && errno == ECHILD &&
		        (child_events & 8), "auto-reap no-cld-wait child");
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_IGN;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGCHLD, &action, 0),
	        "ignore child signal");
	for (round = 0; round < 8; round++) {
		child = fork();
		require(child >= 0, "fork ignored child");
		if (!child)
			_exit(0);
		errno = 0;
		require(waitpid(child, &status, 0) == -1 && errno == ECHILD,
		        "auto-reap ignored child");
	}
	action.sa_handler = SIG_DFL;
	require(!sigaction(SIGCHLD, &action, 0),
	        "restore child disposition");
}

static void test_fault_and_reset(void)
{
	struct sigaction action = { 0 };
	pid_t child;
	int status;

	child = fork();
	require(child >= 0, "fork fault child");
	if (!child) {
		action.sa_sigaction = fault_handler;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGSEGV, &action, 0))
			_exit(101);
		__asm__ volatile("sw zero, 0(%0)" :: "r"(0x1000UL) : "memory");
		_exit(102);
	}
	require(waitpid(child, &status, 0) == child, "wait fault child");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		printf("SIGNAL_FAULT_STATUS %#x\n", status);
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	        "handle synchronous fault");

	child = fork();
	require(child >= 0, "fork illegal child");
	if (!child) {
		__asm__ volatile(".word 0");
		_exit(103);
	}
	require(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
	        WTERMSIG(status) == SIGILL, "report illegal instruction");

	child = fork();
	require(child >= 0, "fork reset child");
	if (!child) {
		action.sa_handler = reset_handler;
		action.sa_flags = SA_RESETHAND;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGUSR1, &action, 0) || raise(SIGUSR1))
			_exit(104);
		raise(SIGUSR1);
		_exit(105);
	}
	require(waitpid(child, &status, 0) == child, "wait reset child");
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGUSR1)
		printf("SIGNAL_RESET_STATUS %#x\n", status);
	require(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
	        "reset signal disposition");
}

static void install_interrupt_handler(int flags)
{
	struct sigaction action = { 0 };

	action.sa_handler = interrupt_handler;
	action.sa_flags = flags;
	sigemptyset(&action.sa_mask);
	require(!sigaction(SIGUSR1, &action, 0),
	        "install interrupt handler");
}

static void test_futex_interruption(void)
{
	struct sender_request request;
	pthread_t sender;
	long result;
	int before;

	handler_error = 0;
	install_interrupt_handler(0);
	restart_futex = 0;
	before = interrupt_count;
	request.target = pthread_self();
	request.operation = SENDER_SIGNAL_ONLY;
	request.child = 0;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create futex interrupt sender");
	errno = 0;
	result = syscall(SYS_futex, &restart_futex,
	                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
	require(result == -1 && errno == EINTR,
	        "interrupt futex without restart");
	require(!pthread_join(sender, 0), "join futex interrupt sender");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver futex interrupt signal");

	install_interrupt_handler(SA_RESTART);
	before = interrupt_count;
	request.operation = SENDER_WAKE_FUTEX;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create futex restart sender");
	errno = 0;
	result = syscall(SYS_futex, &restart_futex,
	                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
	require(result == 0, "restart futex wait");
	require(!pthread_join(sender, 0), "join futex restart sender");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver restarted futex signal");

	{
		struct timespec timeout = { .tv_nsec = 200000000 };
		struct timespec start, finish;
		pthread_t target = pthread_self();
		uint64_t elapsed;
		int futex_errno;

		before = interrupt_count;
		require(!pthread_create(&sender, 0, timed_signal_sender, &target),
		        "create timed futex sender");
		require(!clock_gettime(CLOCK_MONOTONIC, &start),
		        "time futex start");
		errno = 0;
		result = syscall(SYS_futex, &restart_futex,
		                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
		                 0, &timeout, 0, 0);
		futex_errno = errno;
		require(!clock_gettime(CLOCK_MONOTONIC, &finish),
		        "time futex finish");
		elapsed = (uint64_t)(finish.tv_sec - start.tv_sec) * 1000;
		if (finish.tv_nsec >= start.tv_nsec)
			elapsed += (finish.tv_nsec - start.tv_nsec) / 1000000;
		else
			elapsed -= (start.tv_nsec - finish.tv_nsec + 999999) /
			           1000000;
		require(result == -1 && futex_errno == ETIMEDOUT && elapsed < 300,
		        "preserve futex timeout deadline");
		require(!pthread_join(sender, 0), "join timed futex sender");
		require(interrupt_count == before + 3 && !handler_error,
		        "deliver timed futex signals");
	}
}

static void test_ppoll_interruption(void)
{
	struct sender_request request;
	pthread_t sender;
	sigset_t block, empty, old, current;
	int before, result;

	install_interrupt_handler(SA_RESTART);
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	require(!pthread_sigmask(SIG_BLOCK, &block, &old),
	        "block ppoll signal");
	before = interrupt_count;
	request.target = pthread_self();
	request.operation = SENDER_SIGNAL_ONLY;
	request.child = 0;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create ppoll sender");
	sigemptyset(&empty);
	errno = 0;
	result = ppoll(0, 0, 0, &empty);
	require(result == -1 && errno == EINTR,
	        "interrupt ppoll despite restart action");
	require(!pthread_join(sender, 0), "join ppoll sender");
	require(!pthread_sigmask(SIG_SETMASK, 0, &current) &&
	        sigismember(&current, SIGUSR1) == 1,
	        "restore ppoll signal mask");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver ppoll signal");
	require(!pthread_sigmask(SIG_SETMASK, &old, 0),
	        "restore ppoll caller mask");
}

static void test_wait_interruption(void)
{
	struct sender_request request;
	pthread_t sender;
	pid_t child, result;
	int before, status;

	install_interrupt_handler(0);
	child = fork();
	require(child >= 0, "fork interrupted wait child");
	if (!child) {
		for (;;)
			__asm__ volatile("" ::: "memory");
	}
	before = interrupt_count;
	request.target = pthread_self();
	request.operation = SENDER_SIGNAL_ONLY;
	request.child = child;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create wait interrupt sender");
	errno = 0;
	result = waitpid(child, &status, 0);
	require(result == -1 && errno == EINTR,
	        "interrupt wait without restart");
	require(!pthread_join(sender, 0), "join wait interrupt sender");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver wait interrupt signal");
	require(!kill(child, SIGTERM), "terminate interrupted wait child");
	require(waitpid(child, &status, 0) == child && WIFSIGNALED(status),
	        "reap interrupted wait child");

	install_interrupt_handler(SA_RESTART);
	child = fork();
	require(child >= 0, "fork restarted wait child");
	if (!child) {
		for (;;)
			__asm__ volatile("" ::: "memory");
	}
	before = interrupt_count;
	request.operation = SENDER_KILL_CHILD;
	request.child = child;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create wait restart sender");
	result = waitpid(child, &status, 0);
	require(result == child && WIFSIGNALED(status) &&
	        WTERMSIG(status) == SIGTERM, "restart wait after handler");
	require(!pthread_join(sender, 0), "join wait restart sender");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver restarted wait signal");
}

static void test_tty_interruption(void)
{
	struct sender_request request;
	pthread_t sender;
	char byte;
	ssize_t result;
	int before;

	install_interrupt_handler(0);
	before = interrupt_count;
	request.target = pthread_self();
	request.operation = SENDER_SIGNAL_ONLY;
	request.child = 0;
	request.observed_count = before;
	require(!pthread_create(&sender, 0, interrupt_sender, &request),
	        "create tty interrupt sender");
	errno = 0;
	result = read(STDIN_FILENO, &byte, sizeof(byte));
	require(result == -1 && errno == EINTR,
	        "interrupt blocking tty read");
	require(!pthread_join(sender, 0), "join tty interrupt sender");
	require(interrupt_count == before + 1 && !handler_error,
	        "deliver tty interrupt signal");
}

static void test_pthread_cancellation(void)
{
	pthread_t thread;
	void *result;

	atomic_store(&cancel_ready, 0);
	require(!pthread_create(&thread, 0, cancel_thread, 0),
	        "create cancellable thread");
	while (!atomic_load_explicit(&cancel_ready, memory_order_acquire))
		;
	require(!pthread_cancel(thread), "cancel blocked thread");
	require(!pthread_join(thread, &result), "join cancelled thread");
	require(result == PTHREAD_CANCELED, "observe pthread cancellation");
}

static void test_exit_group_pressure(void)
{
	pthread_t threads[EXIT_GROUP_THREADS];
	pid_t child;
	int round, status, index;

	for (round = 0; round < EXIT_GROUP_ROUNDS; round++) {
		child = fork();
		require(child >= 0, "fork exit-group child");
		if (!child) {
			atomic_store(&exit_group_ready, 0);
			exit_group_futex = 0;
			for (index = 0; index < EXIT_GROUP_THREADS; index++) {
				if (pthread_create(&threads[index], 0,
				                   exit_group_thread, 0))
					_exit(108);
			}
			while (atomic_load_explicit(&exit_group_ready,
			                            memory_order_acquire) !=
			       EXIT_GROUP_THREADS)
				;
			execl("/bin/signal-runtime", "signal-runtime",
			      "exec-ok", NULL);
			_exit(37);
		}
		require(waitpid(child, &status, 0) == child &&
		        WIFEXITED(status) && !WEXITSTATUS(status),
		        "exec multithreaded process group");
	}
	child = fork();
	require(child >= 0, "fork forced exit child");
	if (!child) {
		atomic_store(&exit_group_ready, 0);
		for (index = 0; index < EXIT_GROUP_THREADS; index++)
			if (pthread_create(&threads[index], 0,
			                   exit_group_socket_thread, 0))
				_exit(107);
		while (atomic_load_explicit(&exit_group_ready,
		                            memory_order_acquire) !=
		       EXIT_GROUP_THREADS)
			;
		if (delay_milliseconds(20))
			_exit(109);
		syscall(SYS_exit_group, 42);
		_exit(110);
	}
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	        WEXITSTATUS(status) == 42,
	        "force noninterruptible group exit");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "exec-reset")) {
		raise(SIGUSR1);
		return 109;
	}
	if (argc == 2 && !strcmp(argv[1], "exec-ignore")) {
		raise(SIGUSR2);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "exec-ok"))
		return 0;
	test_basic_delivery();
	test_register_frame();
	test_pending_and_wait();
	test_default_ignored_wait();
	test_pending_priority_and_realtime();
	test_sigsuspend();
	test_altstack();
	test_thread_targeting();
	test_process_signal_wake();
	test_child_events();
	test_child_auto_reap();
	test_exec_dispositions();
	test_fault_and_reset();
	test_futex_interruption();
	test_ppoll_interruption();
	test_wait_interruption();
	test_tty_interruption();
	test_pthread_cancellation();
	test_exit_group_pressure();
	puts("SIGNAL_RUNTIME_OK");
	return 0;
}
