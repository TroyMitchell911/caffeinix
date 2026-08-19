#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000LL

static volatile sig_atomic_t signal_seen;

static void fail(const char *operation)
{
	printf("TIME_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	exit(1);
}

static int64_t time_ns(const struct timespec *time)
{
	return (int64_t)time->tv_sec * NSEC_PER_SEC + time->tv_nsec;
}

static void require_elapsed(const char *operation,
			    const struct timespec *start,
			    const struct timespec *finish,
			    int64_t minimum, int64_t maximum)
{
	int64_t elapsed = time_ns(finish) - time_ns(start);

	if (elapsed < minimum || elapsed > maximum)
		fail(operation);
}

static void signal_handler(int signal)
{
	(void)signal;
	signal_seen = 1;
}

static void check_clocks(void)
{
	struct timespec realtime, monotonic, resolution;
	struct timeval timeval;
	int64_t difference;

	if (clock_gettime(CLOCK_REALTIME, &realtime) ||
	    clock_gettime(CLOCK_MONOTONIC, &monotonic) ||
	    clock_getres(CLOCK_REALTIME, &resolution) ||
	    gettimeofday(&timeval, 0))
		fail("clock query");
	if (realtime.tv_sec < 1577836800 || monotonic.tv_sec < 0 ||
	    resolution.tv_sec != 0 || resolution.tv_nsec <= 0 ||
	    resolution.tv_nsec > 100000000)
		fail("clock values");
	difference = time_ns(&realtime) -
		     ((int64_t)timeval.tv_sec * NSEC_PER_SEC +
		      (int64_t)timeval.tv_usec * 1000);
	if (difference < -100000000 || difference > 100000000)
		fail("gettimeofday consistency");
}

static void check_sleeps(void)
{
	struct timespec start, finish, request, deadline;

	request.tv_sec = 0;
	request.tv_nsec = 200000000;
	if (clock_gettime(CLOCK_MONOTONIC, &start) ||
	    nanosleep(&request, 0) ||
	    clock_gettime(CLOCK_MONOTONIC, &finish))
		fail("nanosleep");
	require_elapsed("nanosleep duration", &start, &finish,
			150000000, 1000000000);

	request.tv_nsec = 100000000;
	if (clock_gettime(CLOCK_MONOTONIC, &start) ||
	    clock_nanosleep(CLOCK_MONOTONIC, 0, &request, 0) ||
	    clock_gettime(CLOCK_MONOTONIC, &finish))
		fail("relative clock_nanosleep");
	require_elapsed("relative clock_nanosleep duration", &start, &finish,
			50000000, 1000000000);

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		fail("absolute clock read");
	deadline = start;
	deadline.tv_nsec += 100000000;
	if (deadline.tv_nsec >= NSEC_PER_SEC) {
		deadline.tv_sec++;
		deadline.tv_nsec -= NSEC_PER_SEC;
	}
	if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, 0) ||
	    clock_gettime(CLOCK_MONOTONIC, &finish))
		fail("absolute clock_nanosleep");
	require_elapsed("absolute clock_nanosleep duration", &start, &finish,
			50000000, 1000000000);
}

static void check_interruption(void)
{
	struct sigaction action = { 0 };
	struct timespec request = { .tv_sec = 2 };
	struct timespec remaining = { 0 };
	pid_t child;
	int status;

	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, 0))
		fail("sigaction");
	child = fork();
	if (child < 0)
		fail("fork");
	if (!child) {
		struct timespec delay = { .tv_nsec = 100000000 };

		(void)nanosleep(&delay, 0);
		if (kill(getppid(), SIGUSR1))
			_exit(2);
		_exit(0);
	}
	errno = 0;
	if (nanosleep(&request, &remaining) != -1 || errno != EINTR ||
	    !signal_seen || remaining.tv_sec < 1 || remaining.tv_sec > 2)
		fail("interrupted nanosleep");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		fail("signal child");
}

static void check_fallback_clock(void)
{
	struct timespec realtime, monotonic, deadline, finish;
	struct timeval timeval;
	struct stat status;
	int64_t difference, timestamp_age;
	int fd;

	if (clock_gettime(CLOCK_REALTIME, &realtime) ||
	    clock_gettime(CLOCK_MONOTONIC, &monotonic) ||
	    gettimeofday(&timeval, 0))
		fail("fallback clock query");
	if (realtime.tv_sec < 0 || realtime.tv_sec > 3600 ||
	    monotonic.tv_sec < realtime.tv_sec)
		fail("fallback clock values");
	difference = time_ns(&realtime) -
		     ((int64_t)timeval.tv_sec * NSEC_PER_SEC +
		      (int64_t)timeval.tv_usec * 1000);
	if (difference < -100000000 || difference > 100000000)
		fail("fallback gettimeofday");

	deadline = realtime;
	deadline.tv_nsec += 100000000;
	if (deadline.tv_nsec >= NSEC_PER_SEC) {
		deadline.tv_sec++;
		deadline.tv_nsec -= NSEC_PER_SEC;
	}
	if (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &deadline, 0) ||
	    clock_gettime(CLOCK_MONOTONIC, &finish))
		fail("fallback realtime sleep");
	require_elapsed("fallback realtime sleep duration", &monotonic,
			&finish, 50000000, 1000000000);

	fd = open("/tmp/time-fallback", O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0 || fstat(fd, &status) || close(fd) ||
	    clock_gettime(CLOCK_REALTIME, &realtime))
		fail("fallback timestamp");
	timestamp_age = time_ns(&realtime) -
			(int64_t)status.st_mtim.tv_sec * NSEC_PER_SEC -
			status.st_mtim.tv_nsec;
	if (!time_ns(&status.st_mtim) || timestamp_age < 0 ||
	    timestamp_age > NSEC_PER_SEC)
		fail("fallback timestamp value");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "fallback")) {
		check_fallback_clock();
		puts("TIME_FALLBACK_OK");
		return 0;
	}
	if (argc != 1)
		fail("arguments");
	check_clocks();
	check_sleeps();
	check_interruption();
	puts("TIME_RUNTIME_OK");
	return 0;
}
