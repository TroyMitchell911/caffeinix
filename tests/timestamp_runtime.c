#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_ATIME_SECONDS 1700000000
#define TEST_ATIME_NANOSECONDS 123456789
#define TEST_MTIME_SECONDS 1700000100
#define TEST_MTIME_NANOSECONDS 987654321
#define TIMESTAMP_OWNER_RACE "/tmp/timestamp-owner-race"
#define TIMESTAMP_RACE_ROUNDS 256

static void fail(const char *operation)
{
	printf("TIMESTAMP_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	exit(1);
}

static int same_time(const struct timespec *left,
		     const struct timespec *right)
{
	return left->tv_sec == right->tv_sec &&
	       left->tv_nsec == right->tv_nsec;
}

static void set_known_times(const char *path, int flags)
{
	struct timespec times[2] = {
		{ TEST_ATIME_SECONDS, TEST_ATIME_NANOSECONDS },
		{ TEST_MTIME_SECONDS, TEST_MTIME_NANOSECONDS },
	};

	if (utimensat(AT_FDCWD, path, times, flags))
		fail("utimensat known times");
}

static void require_known_times(const char *path, int symlink)
{
	struct stat state;

	if ((symlink ? lstat(path, &state) : stat(path, &state)))
		fail("stat known times");
	if (state.st_atim.tv_sec != TEST_ATIME_SECONDS ||
	    state.st_atim.tv_nsec != TEST_ATIME_NANOSECONDS ||
	    state.st_mtim.tv_sec != TEST_MTIME_SECONDS ||
	    state.st_mtim.tv_nsec != TEST_MTIME_NANOSECONDS)
		fail("known timestamp values");
}

static void make_file(const char *path)
{
	struct stat state;
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);

	if (fd < 0 || write(fd, "timestamp", 9) != 9 ||
	    fstat(fd, &state) || close(fd))
		fail("create timestamp file");
	if (state.st_atim.tv_sec < 1577836800 ||
	    state.st_mtim.tv_sec < 1577836800 ||
	    state.st_ctim.tv_sec < 1577836800)
		fail("automatic creation times");
}

static void test_filesystem(const char *path, int updates_atime)
{
	struct timespec omitted[2];
	struct timespec invalid[2] = { { 0, 1000000000 }, { 0, 0 } };
	struct stat before, after;
	char byte;
	int fd;

	make_file(path);
	set_known_times(path, 0);
	require_known_times(path, 0);
	fd = open(path, O_RDONLY);
	if (fd < 0 || futimens(fd, (struct timespec[2]) {
			{ TEST_ATIME_SECONDS, TEST_ATIME_NANOSECONDS },
			{ TEST_MTIME_SECONDS, TEST_MTIME_NANOSECONDS },
		}) || close(fd))
		fail("futimens");
	require_known_times(path, 0);
	fd = open(path, O_RDONLY);
	if (fd < 0 || fsync(fd) || close(fd))
		fail("sync timestamp file");
	require_known_times(path, 0);
	if (stat(path, &before))
		fail("stat before zero read");
	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, &byte, 0) != 0 || close(fd) ||
	    stat(path, &after))
		fail("zero read timestamp");
	if (!same_time(&before.st_atim, &after.st_atim) ||
	    !same_time(&before.st_mtim, &after.st_mtim) ||
	    !same_time(&before.st_ctim, &after.st_ctim))
		fail("zero read changed timestamps");

	omitted[0].tv_sec = 0;
	omitted[0].tv_nsec = UTIME_OMIT;
	omitted[1] = omitted[0];
	if (stat(path, &before) || utimensat(AT_FDCWD, path, omitted, 0) ||
	    stat(path, &after))
		fail("UTIME_OMIT");
	if (!same_time(&before.st_atim, &after.st_atim) ||
	    !same_time(&before.st_mtim, &after.st_mtim) ||
	    !same_time(&before.st_ctim, &after.st_ctim))
		fail("UTIME_OMIT changed metadata");

	errno = 0;
	if (utimensat(AT_FDCWD, path, invalid, 0) != -1 || errno != EINVAL)
		fail("invalid nanoseconds");

	if (stat(path, &before))
		fail("stat before read");
	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, &byte, 1) != 1 || close(fd) ||
	    stat(path, &after))
		fail("read timestamp update");
	if ((updates_atime &&
	     after.st_atim.tv_sec <= TEST_ATIME_SECONDS) ||
	    (!updates_atime &&
	     !same_time(&before.st_atim, &after.st_atim)) ||
	    !same_time(&before.st_ctim, &after.st_ctim) ||
	    after.st_mtim.tv_sec != TEST_MTIME_SECONDS ||
	    after.st_mtim.tv_nsec != TEST_MTIME_NANOSECONDS)
		fail("read timestamp values");

	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0 || write(fd, "x", 1) != 1 || close(fd) ||
	    stat(path, &after))
		fail("write timestamp update");
	if (after.st_mtim.tv_sec <= TEST_MTIME_SECONDS ||
	    after.st_ctim.tv_sec <= TEST_MTIME_SECONDS)
		fail("write timestamp values");

	set_known_times(path, 0);
	require_known_times(path, 0);
}

static void test_symlink(void)
{
	static const char target[] = "/tmp/timestamp-target";
	static const char link[] = "/tmp/timestamp-link";
	struct stat target_before, target_after;

	make_file(target);
	if (symlink(target, link) || stat(target, &target_before))
		fail("timestamp symlink create");
	set_known_times(link, AT_SYMLINK_NOFOLLOW);
	require_known_times(link, 1);
	if (stat(target, &target_after) ||
	    !same_time(&target_before.st_mtim, &target_after.st_mtim))
		fail("symlink timestamp followed target");
}

static void test_omit_short_circuit(void)
{
	struct timespec omitted[2] = {
		{ .tv_nsec = UTIME_OMIT },
		{ .tv_nsec = UTIME_OMIT },
	};

	if (utimensat(AT_FDCWD, "/missing-omit-target", omitted, 0) ||
	    syscall(SYS_utimensat, AT_FDCWD,
	            (const char *)(uintptr_t)1, omitted, 0) ||
	    syscall(SYS_utimensat, -1, "", omitted, AT_EMPTY_PATH))
		fail("UTIME_OMIT path short circuit");
}

static void test_dirfd(void)
{
	static const char directory[] = "/tmp/timestamp-dirfd";
	static const char path[] = "/tmp/timestamp-dirfd/child";
	struct timespec times[2] = {
		{ TEST_ATIME_SECONDS, TEST_ATIME_NANOSECONDS },
		{ TEST_MTIME_SECONDS, TEST_MTIME_NANOSECONDS },
	};
	struct stat state;
	int directory_fd, file_fd;

	if (mkdir(directory, 0700))
		fail("dirfd directory");
	make_file(path);
	directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 ||
	    utimensat(directory_fd, "child", times, 0) ||
	    stat(path, &state) ||
	    state.st_atim.tv_sec != TEST_ATIME_SECONDS ||
	    state.st_atim.tv_nsec != TEST_ATIME_NANOSECONDS ||
	    state.st_mtim.tv_sec != TEST_MTIME_SECONDS ||
	    state.st_mtim.tv_nsec != TEST_MTIME_NANOSECONDS)
		fail("relative dirfd timestamps");
	file_fd = open(path, O_RDONLY);
	if (file_fd < 0)
		fail("dirfd regular file");
	errno = 0;
	if (utimensat(file_fd, "child", times, 0) != -1 ||
	    errno != ENOTDIR)
		fail("regular dirfd timestamps");
	errno = 0;
	if (utimensat(-1, "child", times, 0) != -1 || errno != EBADF)
		fail("invalid dirfd timestamps");
	if (utimensat(-1, path, times, 0) || close(file_fd) ||
	    close(directory_fd))
		fail("absolute timestamp dirfd");
}

static int timestamp_race_child(int start, int done)
{
	struct timespec times[2] = {
		{ TEST_ATIME_SECONDS, TEST_ATIME_NANOSECONDS },
		{ TEST_MTIME_SECONDS, TEST_MTIME_NANOSECONDS },
	};
	char marker;
	int index;

	if (setgid(1001) || setuid(1001))
		return 1;
	for (index = 0; index < TIMESTAMP_RACE_ROUNDS; index++) {
		if (read(start, &marker, 1) != 1)
			return 2;
		errno = 0;
		if (utimensat(AT_FDCWD, TIMESTAMP_OWNER_RACE, times, 0) &&
		    errno != EPERM)
			return 3;
		if (write(done, &marker, 1) != 1)
			return 4;
	}
	return 0;
}

static void test_owner_change_race(void)
{
	struct timespec owner_times[2] = {
		{ TEST_ATIME_SECONDS + 1000, TEST_ATIME_NANOSECONDS },
		{ TEST_MTIME_SECONDS + 1000, TEST_MTIME_NANOSECONDS },
	};
	struct stat state;
	volatile unsigned int delay;
	char marker = 'R';
	int done[2], index, start[2], status;
	pid_t child;

	unlink(TIMESTAMP_OWNER_RACE);
	make_file(TIMESTAMP_OWNER_RACE);
	if (chown(TIMESTAMP_OWNER_RACE, 1001, 1001) ||
	    pipe(start) || pipe(done))
		fail("timestamp owner race setup");
	child = fork();
	if (child < 0)
		fail("timestamp owner race fork");
	if (!child) {
		close(start[1]);
		close(done[0]);
		_exit(timestamp_race_child(start[0], done[1]));
	}
	close(start[0]);
	close(done[1]);
	for (index = 0; index < TIMESTAMP_RACE_ROUNDS; index++) {
		if (utimensat(AT_FDCWD, TIMESTAMP_OWNER_RACE,
			      owner_times, 0) ||
		    write(start[1], &marker, 1) != 1)
			fail("timestamp owner race start");
		for (delay = 0; delay < (unsigned int)(index & 63) * 32;
		     delay++)
			;
		if (chown(TIMESTAMP_OWNER_RACE, 0, 0) ||
		    utimensat(AT_FDCWD, TIMESTAMP_OWNER_RACE,
			      owner_times, 0) ||
		    read(done[0], &marker, 1) != 1 ||
		    stat(TIMESTAMP_OWNER_RACE, &state) ||
		    !same_time(&state.st_atim, &owner_times[0]) ||
		    !same_time(&state.st_mtim, &owner_times[1]) ||
		    chown(TIMESTAMP_OWNER_RACE, 1001, 1001))
			fail("timestamp owner race serialization");
	}
	close(start[1]);
	close(done[0]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) || unlink(TIMESTAMP_OWNER_RACE))
		fail("timestamp owner race child");
}

int main(void)
{
	test_filesystem("/timestamp-runtime", 0);
	test_filesystem("/tmp/timestamp-runtime", 1);
	test_symlink();
	test_omit_short_circuit();
	test_dirfd();
	test_owner_change_race();
	puts("TIMESTAMP_RUNTIME_OK");
	return 0;
}
