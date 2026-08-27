#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_PIPE_CAPACITY 4096
#define TRANSFER_SIZE      (TEST_PIPE_CAPACITY * 4 + 37)
#define WRITER_COUNT       4
#define RECORD_COUNT       32
#define RECORD_SIZE        64

struct record {
	unsigned int writer;
	unsigned int sequence;
	unsigned char payload[RECORD_SIZE - 2 * sizeof(unsigned int)];
};

_Static_assert(sizeof(struct record) == RECORD_SIZE, "record size");

static volatile sig_atomic_t pipe_signals;

struct pipe_signal_sibling {
	sigset_t signals;
	atomic_int failed;
	atomic_int observed;
	atomic_int probe;
	atomic_int ready;
	atomic_int stop;
};

static int fail(const char *operation)
{
	printf("PIPE_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	return 1;
}

static int close_pair(int descriptors[2])
{
	int result = 0;

	if (descriptors[0] >= 0 && close(descriptors[0]) < 0)
		result = -1;
	if (descriptors[1] >= 0 && close(descriptors[1]) < 0)
		result = -1;
	return result;
}

static int write_all(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result = write(descriptor, bytes + offset,
		                       length - offset);

		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!result)
			return -1;
		offset += result;
	}
	return 0;
}

static int read_all(int descriptor, void *buffer, size_t length)
{
	unsigned char *bytes = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result = read(descriptor, bytes + offset,
		                      length - offset);

		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!result)
			return -1;
		offset += result;
	}
	return 0;
}

static int wait_success(pid_t child)
{
	int status;

	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int test_metadata(void)
{
	struct stat read_status, write_status;
	int descriptors[2] = { -1, -1 };

	if (pipe(descriptors) || fstat(descriptors[0], &read_status) ||
	    fstat(descriptors[1], &write_status) ||
	    !S_ISFIFO(read_status.st_mode) ||
	    !S_ISFIFO(write_status.st_mode) || !read_status.st_ino ||
	    read_status.st_ino != write_status.st_ino ||
	    read_status.st_uid != geteuid() ||
	    write_status.st_uid != geteuid() ||
	    read_status.st_gid != getegid() ||
	    write_status.st_gid != getegid() || close_pair(descriptors))
		return fail("pipe metadata");
	return 0;
}

static int test_nonblocking(void)
{
	struct pollfd poll_descriptors[2];
	char *buffer;
	int descriptors[2] = { -1, -1 };
	int flags;
	char byte;

	if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) < 0)
		return fail("pipe2 flags");
	flags = fcntl(descriptors[0], F_GETFD);
	if (flags < 0 || !(flags & FD_CLOEXEC))
		return fail("read close-on-exec");
	flags = fcntl(descriptors[1], F_GETFD);
	if (flags < 0 || !(flags & FD_CLOEXEC))
		return fail("write close-on-exec");
	flags = fcntl(descriptors[0], F_GETFL);
	if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
	    !(flags & O_NONBLOCK))
		return fail("read flags");
	flags = fcntl(descriptors[1], F_GETFL);
	if (flags < 0 || (flags & O_ACCMODE) != O_WRONLY ||
	    !(flags & O_NONBLOCK))
		return fail("write flags");
	poll_descriptors[0].fd = descriptors[0];
	poll_descriptors[0].events = POLLIN;
	poll_descriptors[1].fd = descriptors[1];
	poll_descriptors[1].events = POLLOUT;
	if (poll(poll_descriptors, 2, 0) != 1 ||
	    poll_descriptors[0].revents ||
	    poll_descriptors[1].revents != POLLOUT)
		return fail("empty poll");
	errno = 0;
	if (read(descriptors[0], &byte, 1) != -1 || errno != EAGAIN)
		return fail("empty nonblocking read");
	buffer = malloc(TEST_PIPE_CAPACITY + 1);
	if (!buffer)
		return fail("nonblocking buffer");
	memset(buffer, 0x5a, TEST_PIPE_CAPACITY + 1);
	if (write(descriptors[1], buffer, TEST_PIPE_CAPACITY) !=
	    TEST_PIPE_CAPACITY)
		return fail("fill pipe");
	errno = 0;
	if (write(descriptors[1], buffer, 1) != -1 || errno != EAGAIN)
		return fail("full nonblocking write");
	if (read(descriptors[0], &byte, 1) != 1 || byte != 0x5a)
		return fail("make pipe space");
	errno = 0;
	if (write(descriptors[1], buffer, 2) != -1 || errno != EAGAIN)
		return fail("atomic nonblocking write");
	if (write(descriptors[1], buffer,
	          TEST_PIPE_CAPACITY + 1) != 1)
		return fail("partial nonblocking write");
	if (close(descriptors[1]) < 0)
		return fail("close writer");
	descriptors[1] = -1;
	poll_descriptors[0].revents = 0;
	if (poll(poll_descriptors, 1, 0) != 1 ||
	    (poll_descriptors[0].revents & (POLLIN | POLLHUP)) !=
	    (POLLIN | POLLHUP))
		return fail("data and hangup poll");
	free(buffer);
	if (close_pair(descriptors) < 0)
		return fail("nonblocking close");
	return 0;
}

static int test_nonblocking_large_writev(void)
{
	char fill[TEST_PIPE_CAPACITY];
	char first[TEST_PIPE_CAPACITY];
	char output[TEST_PIPE_CAPACITY];
	char second = 'b';
	struct iovec vectors[2] = {
		{ .iov_base = first, .iov_len = sizeof(first) },
		{ .iov_base = &second, .iov_len = sizeof(second) },
	};
	int descriptors[2] = { -1, -1 };
	ssize_t result;

	memset(fill, 'f', sizeof(fill));
	memset(first, 'a', sizeof(first));
	if (pipe2(descriptors, O_NONBLOCK) ||
	    write(descriptors[1], fill, sizeof(fill)) != sizeof(fill) ||
	    read(descriptors[0], output, sizeof(output) / 2) !=
	    sizeof(output) / 2)
		return fail("large writev setup");
	result = writev(descriptors[1], vectors, 2);
	if (result != TEST_PIPE_CAPACITY / 2)
		return fail("large writev partial write");
	if (read(descriptors[0], output, sizeof(output)) != sizeof(output) ||
	    memcmp(output, fill + sizeof(fill) / 2, sizeof(fill) / 2) ||
	    memcmp(output + sizeof(output) / 2, first,
	           sizeof(first) / 2))
		return fail("large writev contents");
	if (close_pair(descriptors))
		return fail("large writev close");
	return 0;
}

static int test_vectored_eof(void)
{
	const char first[] = "ab";
	const char second[] = "cde";
	struct iovec vectors[2];
	struct pollfd poll_descriptor;
	char output_first[2], output_second[3], byte;
	int descriptors[2] = { -1, -1 };

	if (pipe(descriptors) < 0)
		return fail("vectored pipe");
	vectors[0].iov_base = (void *)first;
	vectors[0].iov_len = sizeof(first) - 1;
	vectors[1].iov_base = (void *)second;
	vectors[1].iov_len = sizeof(second) - 1;
	if (writev(descriptors[1], vectors, 2) != 5 ||
	    close(descriptors[1]) < 0)
		return fail("vectored write");
	descriptors[1] = -1;
	vectors[0].iov_base = output_first;
	vectors[0].iov_len = sizeof(output_first);
	vectors[1].iov_base = output_second;
	vectors[1].iov_len = sizeof(output_second);
	if (readv(descriptors[0], vectors, 2) != 5 ||
	    memcmp(output_first, first, sizeof(output_first)) ||
	    memcmp(output_second, second, sizeof(output_second)))
		return fail("vectored read");
	if (read(descriptors[0], &byte, 1) != 0)
		return fail("pipe EOF");
	poll_descriptor.fd = descriptors[0];
	poll_descriptor.events = POLLIN;
	poll_descriptor.revents = 0;
	if (poll(&poll_descriptor, 1, 0) != 1 ||
	    poll_descriptor.revents != POLLHUP)
		return fail("EOF poll");
	if (close_pair(descriptors) < 0)
		return fail("vectored close");
	return 0;
}

static int test_faulting_read(void)
{
	static const char payload[] = "faulting pipe read";
	char *mapping;
	int descriptors[2] = { -1, -1 };

	mapping = mmap(NULL, TEST_PIPE_CAPACITY, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || pipe(descriptors) ||
	    write(descriptors[1], payload, sizeof(payload)) !=
	    sizeof(payload) ||
	    read(descriptors[0], mapping, sizeof(payload)) != sizeof(payload) ||
	    memcmp(mapping, payload, sizeof(payload)))
		return fail("faulting read");
	if (write(descriptors[1], payload, sizeof(payload)) !=
	    sizeof(payload) ||
	    mprotect(mapping, TEST_PIPE_CAPACITY, PROT_NONE))
		return fail("faulting read protect");
	errno = 0;
	if (read(descriptors[0], mapping, sizeof(payload)) != -1 ||
	    errno != EFAULT ||
	    mprotect(mapping, TEST_PIPE_CAPACITY, PROT_READ | PROT_WRITE) ||
	    read(descriptors[0], mapping, sizeof(payload)) != sizeof(payload) ||
	    memcmp(mapping, payload, sizeof(payload)) ||
	    munmap(mapping, TEST_PIPE_CAPACITY) || close_pair(descriptors))
		return fail("faulting read recovery");
	return 0;
}

static void pipe_handler(int signal)
{
	if (signal == SIGPIPE || signal == SIGUSR1)
		pipe_signals++;
}

static int test_restarted_write(int vectored)
{
	struct sigaction action, previous;
	struct iovec vectors[2];
	char buffer[TEST_PIPE_CAPACITY] = { 0 };
	char bytes[2] = { 'a', 'b' };
	int completion[2] = { -1, -1 };
	int descriptors[2] = { -1, -1 };
	int count = vectored ? 2 : 1;
	int completion_result, result, status;
	pid_t child;

	memset(&action, 0, sizeof(action));
	action.sa_handler = pipe_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, &previous) || pipe(descriptors) ||
	    pipe(completion) ||
	    write(descriptors[1], buffer, sizeof(buffer)) != sizeof(buffer))
		return -1;
	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		int failed = 0;

		close(descriptors[1]);
		close(completion[1]);
		usleep(20000);
		if (kill(getppid(), SIGUSR1))
			failed = 1;
		usleep(20000);
		if (read(descriptors[0], buffer, count) != count ||
		    read(completion[0], buffer, 1) != 1)
			failed = 1;
		close(completion[0]);
		close(descriptors[0]);
		_exit(failed);
	}
	close(descriptors[0]);
	descriptors[0] = -1;
	close(completion[0]);
	completion[0] = -1;
	pipe_signals = 0;
	vectors[0].iov_base = &bytes[0];
	vectors[0].iov_len = 1;
	vectors[1].iov_base = &bytes[1];
	vectors[1].iov_len = 1;
	result = vectored ? writev(descriptors[1], vectors, 2) :
		write(descriptors[1], bytes, 1);
	completion_result = write(completion[1], bytes, 1);
	if (close(completion[1]) || close(descriptors[1]) ||
	    waitpid(child, &status, 0) != child || completion_result != 1 ||
	    !WIFEXITED(status) || WEXITSTATUS(status) || result != count ||
	    pipe_signals != 1 || sigaction(SIGUSR1, &previous, NULL))
		return -1;
	return 0;
}

static void *pipe_signal_sibling(void *argument)
{
	struct pipe_signal_sibling *sibling = argument;

	if (pthread_sigmask(SIG_UNBLOCK, &sibling->signals, NULL))
		atomic_store_explicit(&sibling->failed, 1,
				      memory_order_relaxed);
	atomic_store_explicit(&sibling->ready, 1, memory_order_release);
	while (!atomic_load_explicit(&sibling->stop, memory_order_acquire)) {
		if (atomic_exchange_explicit(&sibling->probe, 0,
					     memory_order_acq_rel))
			atomic_store_explicit(&sibling->observed, 1,
					      memory_order_release);
		atomic_signal_fence(memory_order_seq_cst);
	}
	return NULL;
}

static int test_thread_directed_broken_pipe(void)
{
	struct pipe_signal_sibling sibling;
	struct sigaction action, previous_action;
	sigset_t pending, previous_mask;
	pthread_t thread;
	int descriptors[2] = { -1, -1 };
	char byte = 1;
	int created = 0, failed = 0;

	memset(&action, 0, sizeof(action));
	action.sa_handler = pipe_handler;
	sigemptyset(&action.sa_mask);
	sigemptyset(&sibling.signals);
	sigaddset(&sibling.signals, SIGPIPE);
	atomic_init(&sibling.failed, 0);
	atomic_init(&sibling.observed, 0);
	atomic_init(&sibling.probe, 0);
	atomic_init(&sibling.ready, 0);
	atomic_init(&sibling.stop, 0);
	if (sigaction(SIGPIPE, &action, &previous_action) ||
	    pthread_sigmask(SIG_BLOCK, &sibling.signals, &previous_mask))
		return -1;
	if (pthread_create(&thread, NULL, pipe_signal_sibling, &sibling)) {
		failed = 1;
		goto out;
	}
	created = 1;
	while (!atomic_load_explicit(&sibling.ready, memory_order_acquire))
		atomic_signal_fence(memory_order_seq_cst);
	if (atomic_load_explicit(&sibling.failed, memory_order_relaxed) ||
	    pipe(descriptors) || close(descriptors[0])) {
		failed = 1;
		goto out;
	}
	descriptors[0] = -1;
	pipe_signals = 0;
	errno = 0;
	if (write(descriptors[1], &byte, 1) != -1 || errno != EPIPE) {
		failed = 1;
		goto out;
	}
	atomic_store_explicit(&sibling.probe, 1, memory_order_release);
	while (!atomic_load_explicit(&sibling.observed,
				     memory_order_acquire))
		atomic_signal_fence(memory_order_seq_cst);
	if (pipe_signals || sigpending(&pending) ||
	    !sigismember(&pending, SIGPIPE) ||
	    pthread_sigmask(SIG_UNBLOCK, &sibling.signals, NULL)) {
		failed = 1;
		goto out;
	}
	if (pipe_signals != 1)
		failed = 1;
out:
	if (created) {
		atomic_store_explicit(&sibling.stop, 1, memory_order_release);
		pthread_join(thread, NULL);
	}
	if (descriptors[0] >= 0)
		close(descriptors[0]);
	if (descriptors[1] >= 0)
		close(descriptors[1]);
	if (pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) ||
	    sigaction(SIGPIPE, &previous_action, NULL))
		failed = 1;
	return failed ? -1 : 0;
}

static int test_broken_pipe(void)
{
	struct sigaction action, previous;
	struct pollfd poll_descriptor;
	int descriptors[2] = { -1, -1 };
	char byte = 1;

	memset(&action, 0, sizeof(action));
	action.sa_handler = pipe_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPIPE, &action, &previous) < 0 ||
	    pipe(descriptors) < 0 || close(descriptors[0]) < 0)
		return fail("broken pipe setup");
	descriptors[0] = -1;
	poll_descriptor.fd = descriptors[1];
	poll_descriptor.events = POLLOUT;
	poll_descriptor.revents = 0;
	if (poll(&poll_descriptor, 1, 0) != 1 ||
	    poll_descriptor.revents != POLLERR)
		return fail("broken pipe poll");
	pipe_signals = 0;
	errno = 0;
	if (write(descriptors[1], &byte, 1) != -1 || errno != EPIPE ||
	    pipe_signals != 1)
		return fail("SIGPIPE");
	if (close_pair(descriptors) < 0 ||
	    sigaction(SIGPIPE, &previous, 0) < 0)
		return fail("broken pipe close");
	return 0;
}

static int test_blocking_transfer(void)
{
	unsigned char *expected, *received;
	int descriptors[2] = { -1, -1 };
	pid_t child;
	int index;

	expected = malloc(TRANSFER_SIZE);
	received = malloc(TRANSFER_SIZE);
	if (!expected || !received)
		return fail("transfer buffers");
	for (index = 0; index < TRANSFER_SIZE; index++)
		expected[index] = index * 17 + 3;
	if (pipe(descriptors) < 0)
		return fail("blocking pipe");
	child = fork();
	if (child < 0)
		return fail("blocking fork");
	if (!child) {
		char extra;

		close(descriptors[1]);
		if (read_all(descriptors[0], received, TRANSFER_SIZE) < 0 ||
		    memcmp(expected, received, TRANSFER_SIZE) ||
		    read(descriptors[0], &extra, 1) != 0)
			_exit(1);
		close(descriptors[0]);
		_exit(0);
	}
	close(descriptors[0]);
	descriptors[0] = -1;
	if (write_all(descriptors[1], expected, TRANSFER_SIZE) < 0 ||
	    close(descriptors[1]) < 0 || wait_success(child) < 0)
		return fail("blocking transfer");
	descriptors[1] = -1;
	free(received);
	free(expected);
	return 0;
}

static int writer_process(int descriptor, unsigned int writer)
{
	struct record record;
	struct iovec vectors[2];
	unsigned int sequence;

	for (sequence = 0; sequence < RECORD_COUNT; sequence++) {
		record.writer = writer;
		record.sequence = sequence;
		memset(record.payload, 'A' + writer,
		       sizeof(record.payload));
		vectors[0].iov_base = &record;
		vectors[0].iov_len = 2 * sizeof(unsigned int);
		vectors[1].iov_base = record.payload;
		vectors[1].iov_len = sizeof(record.payload);
		if (writev(descriptor, vectors, 2) != sizeof(record))
			return -1;
	}
	return 0;
}

static int test_atomic_stress(void)
{
	unsigned char seen[WRITER_COUNT][RECORD_COUNT] = { { 0 } };
	pid_t children[WRITER_COUNT];
	int descriptors[2] = { -1, -1 };
	unsigned int index;

	if (pipe(descriptors) < 0)
		return fail("stress pipe");
	for (index = 0; index < WRITER_COUNT; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return fail("stress fork");
		if (!children[index]) {
			close(descriptors[0]);
			if (writer_process(descriptors[1], index) < 0)
				_exit(1);
			close(descriptors[1]);
			_exit(0);
		}
	}
	close(descriptors[1]);
	descriptors[1] = -1;
	for (index = 0; index < WRITER_COUNT * RECORD_COUNT; index++) {
		struct record record;
		struct iovec vectors[2] = {
			{ &record, 2 * sizeof(unsigned int) },
			{ record.payload, sizeof(record.payload) },
		};
		unsigned int byte;

		if (readv(descriptors[0], vectors, 2) != sizeof(record) ||
		    record.writer >= WRITER_COUNT ||
		    record.sequence >= RECORD_COUNT ||
		    seen[record.writer][record.sequence])
			return fail("atomic record header");
		for (byte = 0; byte < sizeof(record.payload); byte++) {
			if (record.payload[byte] != 'A' + record.writer)
				return fail("atomic record payload");
		}
		seen[record.writer][record.sequence] = 1;
	}
	if (close(descriptors[0]) < 0)
		return fail("stress read close");
	for (index = 0; index < WRITER_COUNT; index++) {
		if (wait_success(children[index]) < 0)
			return fail("stress child");
	}
	return 0;
}

static int check_closed_descriptors(const char *first, const char *second)
{
	int descriptors[2] = { atoi(first), atoi(second) };
	int index;

	for (index = 0; index < 2; index++) {
		errno = 0;
		if (fcntl(descriptors[index], F_GETFD) != -1 || errno != EBADF)
			return 1;
	}
	return 0;
}

static int test_close_on_exec(void)
{
	char first[16], second[16];
	int descriptors[2] = { -1, -1 };
	pid_t child;

	if (pipe2(descriptors, O_CLOEXEC) < 0)
		return fail("exec pipe");
	child = fork();
	if (child < 0)
		return fail("exec fork");
	if (!child) {
		snprintf(first, sizeof(first), "%d", descriptors[0]);
		snprintf(second, sizeof(second), "%d", descriptors[1]);
		execl("/bin/pipe-runtime", "pipe-runtime", "--check-closed",
		      first, second, (char *)0);
		_exit(127);
	}
	if (close_pair(descriptors) < 0 || wait_success(child) < 0)
		return fail("close-on-exec");
	return 0;
}

int main(int argc, char **argv)
{
	int descriptors[2];

	if (argc == 4 && !strcmp(argv[1], "--check-closed"))
		return check_closed_descriptors(argv[2], argv[3]);
	errno = 0;
	if (pipe2(descriptors, O_APPEND) != -1 || errno != EINVAL)
		return fail("invalid flags");
	if (test_metadata() || test_nonblocking() ||
	    test_nonblocking_large_writev() ||
	    test_vectored_eof() || test_faulting_read() ||
	    test_broken_pipe() || test_thread_directed_broken_pipe() ||
	    test_restarted_write(0) || test_restarted_write(1) ||
	    test_blocking_transfer() ||
	    test_atomic_stress() || test_close_on_exec())
		return 1;
	puts("PIPE_RUNTIME_OK");
	return 0;
}
