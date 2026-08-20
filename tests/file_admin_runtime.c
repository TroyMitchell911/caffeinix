#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLKSSZGET    0x1268
#define BLKGETSIZE64 0x80081272
#define EXT4_MAGIC   0xef53
#define TMPFS_MAGIC  0x01021994

#define ADMIN_FILE       "/file-admin"
#define ADMIN_FIFO       "/file-admin-fifo"
#define ADMIN_SPACE      "/file-admin-space"
#define ADMIN_TMP_FIFO   "/tmp/file-admin-fifo"
#define ADMIN_TMP_SPACE  "/tmp/file-admin-space"
#define ADMIN_SETID_WRITE "/tmp/file-admin-setid-write"
#define ADMIN_SETID_TRUNCATE "/tmp/file-admin-setid-truncate"
#define ADMIN_SETID_MMAP  "/tmp/file-admin-setid-mmap"
#define ADMIN_SETID_MMAP_READ "/tmp/file-admin-setid-mmap-read"
#define ADMIN_SETID_MMAP_POPULATE "/tmp/file-admin-setid-mmap-populate"
#define ADMIN_SETID_MPROTECT "/tmp/file-admin-setid-mprotect"
#define ADMIN_SETID_FALLOCATE "/tmp/file-admin-setid-fallocate"
#define ADMIN_SETID_READONLY "/tmp/file-admin-setid-readonly"
#define ADMIN_DIRFD_FILE  "/tmp/file-admin-dirfd"
#define ADMIN_MKNOD_DIR   "/tmp/file-admin-mknodat"
#define ADMIN_MKNOD_FILE  ADMIN_MKNOD_DIR "/regular"
#define ADMIN_MKNOD_CHAR  ADMIN_MKNOD_DIR "/character"
#define ADMIN_MKNOD_SOCK  ADMIN_MKNOD_DIR "/socket"
#define ADMIN_MKNOD_ABS   "/tmp/file-admin-mknodat-absolute"
#define ADMIN_BAD_BLOCK  "/tmp/file-admin-block"
#define ADMIN_WIDE_MAJOR "/file-admin-wide-major"
#define ADMIN_WIDE_MINOR "/file-admin-wide-minor"
#define ADMIN_USER_BLOCK "/tmp/file-admin-user-block"
#define ADMIN_USER_CHAR  "/tmp/file-admin-user-char"
#define ADMIN_ALIAS_DIR "/tmp/file-admin-alias"
#define ADMIN_ALIAS_CHILD ADMIN_ALIAS_DIR "/child"
#define ADMIN_METADATA_RACE "/tmp/file-admin-metadata-race"
#define ADMIN_SETID_MMAP_RACE "/file-admin-setid-mmap-race"
#define METADATA_RACE_ROUNDS 256
#define SETID_MMAP_RACE_ROUNDS 256
#define RAW_READ_ROUNDS  256
#define RAW_WRITE_ROUNDS 32

struct raw_read_test {
	pthread_barrier_t barrier;
	atomic_int start;
	atomic_int abort;
};

struct raw_read_worker {
	struct raw_read_test *test;
	const unsigned char *expected;
	off_t offset;
	int fd;
	int failed;
};

struct raw_write_test {
	pthread_barrier_t barrier;
	atomic_int start;
	atomic_int abort;
};

struct raw_write_worker {
	struct raw_write_test *test;
	off_t offset;
	unsigned char value;
	int fd;
	int failed;
};

static int fail(const char *step)
{
	printf("FILE_ADMIN_RUNTIME_FAIL %s errno=%d\n", step, errno);
	return 1;
}

static void *raw_read_worker(void *argument)
{
	struct raw_read_worker *worker = argument;
	unsigned char observed[512];
	int round;

	while (!atomic_load_explicit(&worker->test->start,
	                            memory_order_acquire))
		usleep(1000);
	if (atomic_load_explicit(&worker->test->abort,
	                         memory_order_relaxed))
		return NULL;
	for (round = 0; round < RAW_READ_ROUNDS; round++) {
		pthread_barrier_wait(&worker->test->barrier);
		if (pread(worker->fd, observed, sizeof(observed),
		          worker->offset) != sizeof(observed) ||
		    memcmp(observed, worker->expected, sizeof(observed)))
			worker->failed = 1;
		pthread_barrier_wait(&worker->test->barrier);
	}
	return NULL;
}

static int check_concurrent_raw_reads(const char *path, uint32_t sector_size)
{
	unsigned char expected[2][512];
	struct raw_read_worker workers[2];
	struct raw_read_test test;
	pthread_t threads[2];
	int created = 0, failed = 0, fd = -1;

	if (sector_size != sizeof(expected[0]))
		return -1;
	fd = open(path, O_RDONLY);
	workers[0].fd = fd;
	workers[1].fd = fd >= 0 ? dup(fd) : -1;
	if (fd < 0 || workers[1].fd < 0 ||
	    pread(fd, expected[0], sizeof(expected[0]), 0) !=
	    sizeof(expected[0]) ||
	    pread(fd, expected[1], sizeof(expected[1]), 2 * sector_size) !=
	    sizeof(expected[1]) ||
	    !memcmp(expected[0], expected[1], sizeof(expected[0])) ||
	    pthread_barrier_init(&test.barrier, NULL, 2))
		goto out;
	atomic_init(&test.start, 0);
	atomic_init(&test.abort, 0);
	for (int index = 0; index < 2; index++) {
		workers[index].test = &test;
		workers[index].expected = expected[index];
		workers[index].offset = index * 2 * sector_size;
		workers[index].failed = 0;
		if (pthread_create(&threads[index], NULL, raw_read_worker,
		                   &workers[index])) {
			atomic_store_explicit(&test.abort, 1,
			                      memory_order_relaxed);
			atomic_store_explicit(&test.start, 1,
			                      memory_order_release);
			goto destroy_barrier;
		}
		created++;
	}
	atomic_store_explicit(&test.start, 1, memory_order_release);
	for (int index = 0; index < 2; index++)
		pthread_join(threads[index], NULL);
	created = 0;
	if (workers[0].failed || workers[1].failed)
		failed = 1;
	pthread_barrier_destroy(&test.barrier);
	close(workers[1].fd);
	close(fd);
	return failed ? -1 : 0;

destroy_barrier:
	for (int index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
	pthread_barrier_destroy(&test.barrier);
out:
	if (workers[1].fd >= 0)
		close(workers[1].fd);
	if (fd >= 0)
		close(fd);
	return -1;
}

static int check_fallocate(const char *path)
{
	static const char payload[] = "preserved";
	unsigned char zeros[32];
	struct stat stat_buffer;
	char buffer[sizeof(payload)];
	int fd;

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		return -1;
	if (pwrite(fd, payload, sizeof(payload), 1024) != sizeof(payload) ||
	    fallocate(fd, 0, 0, 8192) || close(fd))
		return -1;
	fd = open(path, O_WRONLY);
	if (fd < 0 || fallocate(fd, 0, 0, 12288) ||
	    fstat(fd, &stat_buffer) || stat_buffer.st_size != 12288 ||
	    fallocate(fd, 0, INT64_MAX - 1024, 2048) != -1 ||
	    errno != EINVAL || fstat(fd, &stat_buffer) ||
	    stat_buffer.st_size != 12288 || close(fd))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0 ||
	    pread(fd, buffer, sizeof(buffer), 1024) != sizeof(buffer) ||
	    memcmp(buffer, payload, sizeof(payload)) ||
	    pread(fd, zeros, sizeof(zeros), 4096) != sizeof(zeros)) {
		close(fd);
		return -1;
	}
	for (size_t index = 0; index < sizeof(zeros); index++) {
		if (zeros[index]) {
			close(fd);
			return -1;
		}
	}
	return close(fd);
}

static int check_setid_mutation(void)
{
	static const char *const paths[] = {
		ADMIN_SETID_WRITE,
		ADMIN_SETID_TRUNCATE,
		ADMIN_SETID_MMAP,
		ADMIN_SETID_FALLOCATE,
		ADMIN_SETID_READONLY,
		ADMIN_SETID_MMAP_READ,
		ADMIN_SETID_MMAP_POPULATE,
		ADMIN_SETID_MPROTECT,
	};
	struct stat stat_buffer;
	volatile char observed;
	void *mapping;
	int fd, index, status;
	pid_t child;

	for (index = 0; index < (int)(sizeof(paths) / sizeof(paths[0]));
	     index++) {
		fd = open(paths[index], O_CREAT | O_EXCL | O_WRONLY, 0700);
		if (fd < 0 || write(fd, "data", 4) != 4 || close(fd) ||
		    chown(paths[index], 0, 1001) || chmod(paths[index], 06775))
			goto fail;
	}
	child = fork();
	if (child < 0)
		goto fail;
	if (!child) {
		if (setgid(1001) || setuid(1001))
			_exit(1);
		fd = open(ADMIN_SETID_WRITE, O_WRONLY);
		if (fd < 0 || pwrite(fd, "X", 1, 0) != 1 ||
		    fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) || close(fd))
			_exit(2);
		fd = open(ADMIN_SETID_TRUNCATE, O_WRONLY);
		if (fd < 0 || ftruncate(fd, 1) || fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) || close(fd))
			_exit(3);
		fd = open(ADMIN_SETID_MMAP, O_RDWR);
		if (fd < 0)
			_exit(4);
		mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
		               fd, 0);
		if (mapping == MAP_FAILED)
			_exit(5);
		*(volatile char *)mapping = 'M';
		if (msync(mapping, 4096, MS_SYNC) ||
		    fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) ||
		    munmap(mapping, 4096) || close(fd))
			_exit(6);
		fd = open(ADMIN_SETID_FALLOCATE, O_WRONLY);
		if (fd < 0 || fallocate(fd, 0, 0, 4096) ||
		    fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) || close(fd))
			_exit(7);
		fd = open(ADMIN_SETID_READONLY, O_RDONLY);
		errno = 0;
		if (fd < 0 || write(fd, "X", 1) != -1 || errno != EBADF ||
		    fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) !=
		    (S_ISUID | S_ISGID) || close(fd))
			_exit(8);
		fd = open(ADMIN_SETID_MMAP_READ, O_RDWR);
		mapping = fd < 0 ? MAP_FAILED :
			mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
			     fd, 0);
		if (mapping == MAP_FAILED)
			_exit(9);
		observed = *(volatile char *)mapping;
		(void)observed;
		if (fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) !=
		    (S_ISUID | S_ISGID) || munmap(mapping, 4096) || close(fd))
			_exit(10);
		fd = open(ADMIN_SETID_MMAP_POPULATE, O_RDWR);
		mapping = fd < 0 ? MAP_FAILED :
			mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_POPULATE, fd, 0);
		if (mapping == MAP_FAILED || fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) !=
		    (S_ISUID | S_ISGID) || munmap(mapping, 4096) || close(fd))
			_exit(11);
		fd = open(ADMIN_SETID_MPROTECT, O_RDWR);
		mapping = fd < 0 ? MAP_FAILED :
			mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
		if (mapping == MAP_FAILED)
			_exit(12);
		observed = *(volatile char *)mapping;
		(void)observed;
		if (mprotect(mapping, 4096, PROT_READ | PROT_WRITE) ||
		    fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) !=
		    (S_ISUID | S_ISGID))
			_exit(13);
		*(volatile char *)mapping = 'P';
		if (fstat(fd, &stat_buffer) ||
		    (stat_buffer.st_mode & (S_ISUID | S_ISGID)) ||
		    munmap(mapping, 4096) || close(fd))
			_exit(14);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		goto fail;
	for (index = 0; index < (int)(sizeof(paths) / sizeof(paths[0]));
	     index++) {
		if (unlink(paths[index]))
			return -1;
	}
	return 0;

fail:
	for (index = 0; index < (int)(sizeof(paths) / sizeof(paths[0]));
	     index++)
		unlink(paths[index]);
	return -1;
}

static int setid_mmap_race_child(int start, int done)
{
	volatile unsigned char *mapping;
	char marker = 'R';
	int fd, index;

	if (setgid(1001) || setuid(1001))
		return 1;
	fd = open(ADMIN_SETID_MMAP_RACE, O_RDWR);
	if (fd < 0 || write(done, &marker, 1) != 1)
		return 2;
	for (index = 0; index < SETID_MMAP_RACE_ROUNDS; index++) {
		if (read(start, &marker, 1) != 1)
			return 3;
		mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
		if (mapping == MAP_FAILED)
			return 4;
		mapping[0] = index;
		if (munmap((void *)mapping, 4096) ||
		    write(done, &marker, 1) != 1)
			return 5;
	}
	return close(fd) ? 6 : 0;
}

static int check_setid_mmap_race(void)
{
	struct stat state;
	volatile unsigned int delay;
	char marker = 'R';
	int done[2], fd = -1, index, start[2], status, result = -1;
	pid_t child = -1;

	unlink(ADMIN_SETID_MMAP_RACE);
	fd = open(ADMIN_SETID_MMAP_RACE,
	          O_CREAT | O_EXCL | O_RDWR, 06775);
	if (fd < 0 || ftruncate(fd, 4096) || close(fd))
		goto out;
	fd = -1;
	if (chown(ADMIN_SETID_MMAP_RACE, 0, 1001) ||
	    chmod(ADMIN_SETID_MMAP_RACE, 06775))
		goto out;
	if (pipe(start))
		goto out;
	if (pipe(done))
		goto out_start;
	child = fork();
	if (child < 0)
		goto out_pipes;
	if (!child) {
		close(start[1]);
		close(done[0]);
		_exit(setid_mmap_race_child(start[0], done[1]));
	}
	close(start[0]);
	close(done[1]);
	if (read(done[0], &marker, 1) != 1)
		goto out_parent;
	for (index = 0; index < SETID_MMAP_RACE_ROUNDS; index++) {
		if (chmod(ADMIN_SETID_MMAP_RACE, 06775) ||
		    write(start[1], &marker, 1) != 1)
			goto out_parent;
		for (delay = 0; delay < (unsigned int)(index & 63) * 64;
		     delay++)
			;
		if (chmod(ADMIN_SETID_MMAP_RACE, 04700) ||
		    read(done[0], &marker, 1) != 1 ||
		    stat(ADMIN_SETID_MMAP_RACE, &state) ||
		    (state.st_mode & 0077))
			goto out_parent;
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		goto out_parent;
	child = -1;
	result = 0;
out_parent:
	close(start[1]);
	close(done[0]);
	if (child > 0)
		waitpid(child, &status, 0);
	goto out;
out_pipes:
	close(done[0]);
	close(done[1]);
out_start:
	close(start[0]);
	close(start[1]);
out:
	if (fd >= 0)
		close(fd);
	unlink(ADMIN_SETID_MMAP_RACE);
	return result;
}

static void *raw_write_worker(void *argument)
{
	struct raw_write_worker *worker = argument;
	int round;

	while (!atomic_load_explicit(&worker->test->start,
	                            memory_order_acquire))
		usleep(1000);
	if (atomic_load_explicit(&worker->test->abort,
	                         memory_order_relaxed))
		return NULL;
	for (round = 0; round < RAW_WRITE_ROUNDS; round++) {
		pthread_barrier_wait(&worker->test->barrier);
		if (pwrite(worker->fd, &worker->value, 1, worker->offset) != 1)
			worker->failed = 1;
		pthread_barrier_wait(&worker->test->barrier);
	}
	return NULL;
}

static int check_concurrent_raw_writes(const char *path, uint64_t bytes,
				       uint32_t sector_size)
{
	unsigned char saved[512], cleared[512], observed[512];
	struct raw_write_test test;
	struct raw_write_worker workers[2];
	pthread_t threads[2];
	off_t sector_offset;
	int created = 0, failed = 0, fd = -1, round;

	if (sector_size != sizeof(saved) || bytes < sector_size)
		return -1;
	sector_offset = bytes - sector_size;
	memset(cleared, 0, sizeof(cleared));
	fd = open(path, O_RDWR);
	workers[0].fd = open(path, O_RDWR);
	workers[1].fd = open(path, O_RDWR);
	if (fd < 0 || workers[0].fd < 0 || workers[1].fd < 0 ||
	    pread(fd, saved, sizeof(saved), sector_offset) != sizeof(saved) ||
	    pthread_barrier_init(&test.barrier, NULL, 3))
		goto out;
	atomic_init(&test.start, 0);
	atomic_init(&test.abort, 0);
	workers[0].test = &test;
	workers[0].offset = sector_offset + 13;
	workers[0].value = 0xa5;
	workers[0].failed = 0;
	workers[1].test = &test;
	workers[1].offset = sector_offset + 257;
	workers[1].value = 0x5a;
	workers[1].failed = 0;
	if (pthread_create(&threads[0], NULL, raw_write_worker, &workers[0]))
		goto destroy_barrier;
	created = 1;
	if (pthread_create(&threads[1], NULL, raw_write_worker, &workers[1])) {
		atomic_store_explicit(&test.abort, 1, memory_order_relaxed);
		atomic_store_explicit(&test.start, 1, memory_order_release);
		goto destroy_barrier;
	}
	created = 2;
	atomic_store_explicit(&test.start, 1, memory_order_release);
	for (round = 0; round < RAW_WRITE_ROUNDS; round++) {
		if (pwrite(fd, cleared, sizeof(cleared), sector_offset) !=
		    sizeof(cleared))
			failed = 1;
		pthread_barrier_wait(&test.barrier);
		pthread_barrier_wait(&test.barrier);
		if (pread(fd, observed, sizeof(observed), sector_offset) !=
		    sizeof(observed) || observed[13] != workers[0].value ||
		    observed[257] != workers[1].value)
			failed = 1;
	}
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	created = 0;
	if (workers[0].failed || workers[1].failed)
		failed = 1;
	if (pwrite(fd, saved, sizeof(saved), sector_offset) != sizeof(saved) ||
	    fsync(fd))
		failed = 1;
	pthread_barrier_destroy(&test.barrier);
	close(workers[0].fd);
	close(workers[1].fd);
	close(fd);
	return failed ? -1 : 0;

destroy_barrier:
	if (created)
		pthread_join(threads[0], NULL);
	pthread_barrier_destroy(&test.barrier);
out:
	if (workers[0].fd >= 0)
		close(workers[0].fd);
	if (workers[1].fd >= 0)
		close(workers[1].fd);
	if (fd >= 0)
		close(fd);
	return -1;
}

static int check_fchmodat_dirfd(void)
{
	struct stat stat_buffer;
	int directory_fd, fd;

	fd = open(ADMIN_DIRFD_FILE, O_CREAT | O_EXCL | O_RDONLY, 0600);
	if (fd < 0)
		return -1;
	directory_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 ||
	    fchmodat(directory_fd, "file-admin-dirfd", 0641, 0) ||
	    stat(ADMIN_DIRFD_FILE, &stat_buffer) ||
	    (stat_buffer.st_mode & 07777) != 0641)
		return -1;
	errno = 0;
	if (fchmodat(fd, "child", 0600, 0) != -1 || errno != ENOTDIR)
		return -1;
	errno = 0;
	if (fchmodat(-1, "file-admin-dirfd", 0600, 0) != -1 ||
	    errno != EBADF || fchmodat(-1, ADMIN_DIRFD_FILE, 0642, 0) ||
	    stat(ADMIN_DIRFD_FILE, &stat_buffer) ||
	    (stat_buffer.st_mode & 07777) != 0642 || close(directory_fd) ||
	    close(fd) || unlink(ADMIN_DIRFD_FILE))
		return -1;
	return 0;
}

static int check_fchownat_dirfd(void)
{
	struct stat stat_buffer;
	int directory_fd, fd;

	fd = open(ADMIN_DIRFD_FILE, O_CREAT | O_EXCL | O_RDONLY, 0600);
	if (fd < 0)
		return -1;
	directory_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 ||
	    fchownat(directory_fd, "file-admin-dirfd", 321, 654, 0) ||
	    fchownat(directory_fd, "file-admin-dirfd", -1, -1, 0) ||
	    stat(ADMIN_DIRFD_FILE, &stat_buffer) ||
	    stat_buffer.st_uid != 321 || stat_buffer.st_gid != 654)
		return -1;
	errno = 0;
	if (fchownat(fd, "child", 0, 0, 0) != -1 || errno != ENOTDIR)
		return -1;
	errno = 0;
	if (fchownat(-1, "file-admin-dirfd", -1, -1, 0) != -1 ||
	    errno != EBADF || fchownat(-1, ADMIN_DIRFD_FILE, 123, 456, 0) ||
	    stat(ADMIN_DIRFD_FILE, &stat_buffer) ||
	    stat_buffer.st_uid != 123 || stat_buffer.st_gid != 456 ||
	    close(directory_fd) || close(fd) || unlink(ADMIN_DIRFD_FILE))
		return -1;
	return 0;
}

static int check_mknodat_dirfd(void)
{
	struct stat stat_buffer;
	int directory_fd, file_fd;

	if (mkdir(ADMIN_MKNOD_DIR, 0700))
		return -1;
	directory_fd = open(ADMIN_MKNOD_DIR, O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 ||
	    mknodat(directory_fd, "regular", S_IFREG | 0640, 0) ||
	    stat(ADMIN_MKNOD_FILE, &stat_buffer) ||
	    !S_ISREG(stat_buffer.st_mode) ||
	    mknodat(directory_fd, "character", S_IFCHR | 0600,
		    makedev(1, 3)) ||
	    lstat(ADMIN_MKNOD_CHAR, &stat_buffer) ||
	    !S_ISCHR(stat_buffer.st_mode) || major(stat_buffer.st_rdev) != 1 ||
	    minor(stat_buffer.st_rdev) != 3 ||
	    mknodat(directory_fd, "socket", S_IFSOCK | 0600, 0) ||
	    lstat(ADMIN_MKNOD_SOCK, &stat_buffer) ||
	    !S_ISSOCK(stat_buffer.st_mode))
		return -1;
	file_fd = open(ADMIN_MKNOD_FILE, O_RDONLY);
	if (file_fd < 0)
		return -1;
	errno = 0;
	if (mknodat(file_fd, "child", S_IFREG | 0600, 0) != -1 ||
	    errno != ENOTDIR)
		return -1;
	errno = 0;
	if (mknodat(-1, "relative", S_IFREG | 0600, 0) != -1 ||
	    errno != EBADF ||
	    mknodat(-1, ADMIN_MKNOD_ABS, S_IFREG | 0600, 0) ||
	    close(file_fd) || close(directory_fd) ||
	    unlink(ADMIN_MKNOD_FILE) || unlink(ADMIN_MKNOD_CHAR) ||
	    unlink(ADMIN_MKNOD_SOCK) || unlink(ADMIN_MKNOD_ABS) ||
	    rmdir(ADMIN_MKNOD_DIR))
		return -1;
	return 0;
}

static int check_existing_directory_errors(void)
{
	static const char *const paths[] = {
		"/", ".", "/tmp/.", "/tmp/..",
	};
	size_t index;

	for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
		errno = 0;
		if (mkdir(paths[index], 0700) != -1 || errno != EEXIST)
			return -1;
	}
	return 0;
}

static int check_truncate_path(void)
{
	static const char contents[] = "truncate-path";
	struct stat stat_buffer;
	int fd;

	fd = open("/truncate-path", O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0 || write(fd, contents, sizeof(contents)) != sizeof(contents) ||
	    close(fd) || truncate("/truncate-path", 4096) ||
	    stat("/truncate-path", &stat_buffer) || stat_buffer.st_size != 4096)
		return -1;
	errno = 0;
	if (truncate("/truncate-path", -1) != -1 || errno != EINVAL ||
	    unlink("/truncate-path"))
		return -1;
	return 0;
}

static int check_descriptor_metadata(void)
{
	static const char payload[] = "original descriptor";
	static const char replacement[] = "replacement path";
	struct stat descriptor, renamed, path;
	int fd, replacement_fd;

	fd = open("/descriptor-metadata", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || write(fd, payload, sizeof(payload)) != sizeof(payload) ||
	    rename("/descriptor-metadata", "/descriptor-renamed"))
		return -1;
	replacement_fd = open("/descriptor-metadata",
			      O_CREAT | O_EXCL | O_RDWR, 0600);
	if (replacement_fd < 0 ||
	    write(replacement_fd, replacement, sizeof(replacement)) !=
	    sizeof(replacement) || close(replacement_fd) ||
	    fchmod(fd, 0640) || fchown(fd, 4321, 87) || ftruncate(fd, 4) ||
	    fstat(fd, &descriptor) || stat("/descriptor-renamed", &renamed) ||
	    stat("/descriptor-metadata", &path) ||
	    (descriptor.st_mode & 07777) != 0640 ||
	    descriptor.st_uid != 4321 || descriptor.st_gid != 87 ||
	    descriptor.st_size != 4 || renamed.st_ino != descriptor.st_ino ||
	    renamed.st_size != descriptor.st_size ||
	    (path.st_mode & 07777) != 0600 || path.st_uid != 0 ||
	    path.st_gid != 0 || path.st_size != sizeof(replacement) ||
	    close(fd) || unlink("/descriptor-renamed") ||
	    unlink("/descriptor-metadata"))
		return -1;
	return 0;
}

static int check_block_device(void)
{
	struct stat stat_buffer;
	char path[32];
	uint64_t bytes;
	uint32_t sector_size;
	uint16_t magic;
	int fd, length;

	if (stat("/", &stat_buffer) || !stat_buffer.st_dev)
		return -1;
	length = snprintf(path, sizeof(path), "/dev/virtio-blk%llu",
			  (unsigned long long)stat_buffer.st_dev - 1);
	if (length < 0 || (size_t)length >= sizeof(path) ||
	    stat(path, &stat_buffer) ||
	    !S_ISBLK(stat_buffer.st_mode))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (ioctl(fd, BLKSSZGET, &sector_size) || sector_size != 512 ||
	    ioctl(fd, BLKGETSIZE64, &bytes) || bytes < 8 * 1024 * 1024 ||
	    pread(fd, &magic, sizeof(magic), 1024 + 56) != sizeof(magic) ||
	    magic != EXT4_MAGIC) {
		close(fd);
		return -1;
	}
	if (close(fd))
		return -1;
	fd = open(path, O_WRONLY);
	if (fd < 0 || lseek(fd, bytes, SEEK_SET) != (off_t)bytes ||
	    write(fd, "", 0) || pwrite(fd, "", 0, (off_t)bytes) ||
	    lseek(fd, 0, SEEK_CUR) != (off_t)bytes) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (close(fd))
		return -1;
	if (check_concurrent_raw_reads(path, sector_size))
		return -1;
	return check_concurrent_raw_writes(path, bytes, sector_size);
}

static int check_rejected_block_device(void)
{
	int fd;

	if (mknod(ADMIN_BAD_BLOCK, S_IFBLK | 0600, makedev(1, 0)))
		return -1;
	fd = open(ADMIN_BAD_BLOCK, O_RDONLY);
	if (fd >= 0) {
		close(fd);
		return -1;
	}
	if (errno != ENODEV)
		return -1;
	return unlink(ADMIN_BAD_BLOCK);
}

static int check_unrepresentable_device_nodes(void)
{
	struct stat stat_buffer;

	errno = 0;
	if (mknod(ADMIN_WIDE_MAJOR, S_IFCHR | 0600,
		  makedev(0x1000, 0)) != -1 || errno != EINVAL)
		return -1;
	errno = 0;
	if (lstat(ADMIN_WIDE_MAJOR, &stat_buffer) != -1 || errno != ENOENT)
		return -1;
	errno = 0;
	if (mknod(ADMIN_WIDE_MINOR, S_IFCHR | 0600,
		  makedev(0, 0x100000)) != -1 || errno != EINVAL)
		return -1;
	errno = 0;
	return lstat(ADMIN_WIDE_MINOR, &stat_buffer) == -1 &&
	       errno == ENOENT ? 0 : -1;
}

static int check_unprivileged_device_nodes(void)
{
	struct stat stat_buffer;
	int status;
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (!child) {
		if (setgid(1234) || setuid(1234))
			_exit(1);
		errno = 0;
		if (mknod(ADMIN_USER_BLOCK, S_IFBLK | 0666,
			  makedev(1, 0)) != -1 || errno != EPERM)
			_exit(2);
		errno = 0;
		if (mknod(ADMIN_USER_CHAR, S_IFCHR | 0666,
			  makedev(1, 3)) != -1 || errno != EPERM)
			_exit(3);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return -1;
	errno = 0;
	if (lstat(ADMIN_USER_BLOCK, &stat_buffer) != -1 || errno != ENOENT)
		return -1;
	errno = 0;
	return lstat(ADMIN_USER_CHAR, &stat_buffer) == -1 && errno == ENOENT ?
		0 : -1;
}

static int check_metadata_alias_refresh(void)
{
	int child_status, directory_fd = -1, fd = -1, result = -1;
	pid_t child;

	unlink(ADMIN_ALIAS_CHILD);
	rmdir(ADMIN_ALIAS_DIR);
	if (mkdir(ADMIN_ALIAS_DIR, 0700) ||
	    chown(ADMIN_ALIAS_DIR, 1001, 1001))
		goto out;
	fd = open(ADMIN_ALIAS_CHILD, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0 || close(fd) || chown(ADMIN_ALIAS_CHILD, 1001, 1001))
		goto out;
	fd = -1;
	directory_fd = open(ADMIN_ALIAS_DIR, O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0)
		goto out;
	if (chmod(ADMIN_ALIAS_DIR, 0000))
		goto out;
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		if (setgid(1001) || setuid(1001))
			_exit(1);
		errno = 0;
		if (!fchmodat(directory_fd, "child", 0644, 0))
			_exit(2);
		_exit(errno == EACCES ? 0 : 3);
	}
	if (waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status))
		goto out;
	result = 0;
out:
	if (fd >= 0)
		close(fd);
	if (directory_fd >= 0)
		close(directory_fd);
	chmod(ADMIN_ALIAS_DIR, 0700);
	unlink(ADMIN_ALIAS_CHILD);
	rmdir(ADMIN_ALIAS_DIR);
	return result;
}

static int metadata_race_child(int start, int done)
{
	char command, response;
	int index;

	if (setgid(1001) || setuid(1001))
		return 1;
	for (index = 0; index < METADATA_RACE_ROUNDS; index++) {
		if (read(start, &command, 1) != 1)
			return 2;
		errno = 0;
		if (!chmod(ADMIN_METADATA_RACE, 0644))
			response = 'S';
		else if (errno == EPERM)
			response = 'P';
		else
			return 3;
		if (write(done, &response, 1) != 1)
			return 4;
	}
	return 0;
}

static int check_metadata_authorization_race(void)
{
	struct stat state;
	char command = 'R', response;
	int done[2], fd, index, start[2], status, result = -1;
	pid_t child = -1;

	unlink(ADMIN_METADATA_RACE);
	fd = open(ADMIN_METADATA_RACE,
	          O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0 || close(fd))
		goto out;
	if (pipe(start))
		goto out;
	if (pipe(done))
		goto out_start;
	child = fork();
	if (child < 0)
		goto out_pipes;
	if (!child) {
		close(start[1]);
		close(done[0]);
		_exit(metadata_race_child(start[0], done[1]));
	}
	close(start[0]);
	close(done[1]);
	for (index = 0; index < METADATA_RACE_ROUNDS; index++) {
		if (chown(ADMIN_METADATA_RACE, 1001, 1001) ||
		    chmod(ADMIN_METADATA_RACE, 0600) ||
		    write(start[1], &command, 1) != 1 ||
		    chown(ADMIN_METADATA_RACE, 1002, 1002) ||
		    chmod(ADMIN_METADATA_RACE, 0600) ||
		    read(done[0], &response, 1) != 1 ||
		    (response != 'S' && response != 'P') ||
		    stat(ADMIN_METADATA_RACE, &state) ||
		    (state.st_mode & 0777) != 0600)
			goto out_parent;
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		goto out_parent;
	child = -1;
	result = 0;
out_parent:
	close(start[1]);
	close(done[0]);
	if (child > 0)
		waitpid(child, &status, 0);
	goto out;
out_pipes:
	close(done[0]);
	close(done[1]);
out_start:
	close(start[0]);
	close(start[1]);
out:
	unlink(ADMIN_METADATA_RACE);
	return result;
}

int main(void)
{
	struct statfs filesystem;
	struct stat stat_buffer;
	int fd;

	if (statfs("/", &filesystem) || filesystem.f_type != EXT4_MAGIC ||
	    !filesystem.f_bsize || !filesystem.f_blocks ||
	    filesystem.f_bfree > filesystem.f_blocks)
		return fail("statfs ext4");
	fd = open("/", O_RDONLY | O_DIRECTORY);
	if (fd < 0 || fstatfs(fd, &filesystem) || close(fd) ||
	    filesystem.f_type != EXT4_MAGIC)
		return fail("fstatfs ext4");
	if (statfs("/tmp", &filesystem) ||
	    filesystem.f_type != TMPFS_MAGIC || !filesystem.f_bsize)
		return fail("statfs tmpfs");
	fd = open(ADMIN_FILE, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || close(fd) || chmod(ADMIN_FILE, 06750) ||
	    chown(ADMIN_FILE, 1234, 56) || stat(ADMIN_FILE, &stat_buffer) ||
	    stat_buffer.st_uid != 1234 || stat_buffer.st_gid != 56 ||
	    (stat_buffer.st_mode & 07777) != 0750)
		return fail("metadata update");
	errno = 0;
	if (mkfifo(ADMIN_FIFO, 0640) != -1 || errno != EOPNOTSUPP)
		return fail("ext4 fifo rejection");
	errno = 0;
	if (mkfifo(ADMIN_TMP_FIFO, 0620) != -1 || errno != EOPNOTSUPP)
		return fail("tmpfs fifo rejection");
	if (check_fallocate(ADMIN_SPACE) ||
	    check_fallocate(ADMIN_TMP_SPACE))
		return fail("fallocate");
	if (check_setid_mutation())
		return fail("set-ID mutation");
	if (check_setid_mmap_race())
		return fail("set-ID mmap race");
	if (check_fchmodat_dirfd())
		return fail("fchmodat dirfd");
	if (check_fchownat_dirfd())
		return fail("fchownat dirfd");
	if (check_mknodat_dirfd())
		return fail("mknodat dirfd");
	if (check_existing_directory_errors())
		return fail("existing directory mkdir");
	if (check_truncate_path())
		return fail("truncate path");
	if (check_descriptor_metadata())
		return fail("descriptor metadata");
	if (check_metadata_alias_refresh())
		return fail("metadata alias refresh");
	if (check_metadata_authorization_race())
		return fail("metadata authorization race");
	if (check_block_device())
		return fail("block device");
	if (check_unrepresentable_device_nodes())
		return fail("device encoding");
	if (check_rejected_block_device())
		return fail("unknown block device");
	if (check_unprivileged_device_nodes())
		return fail("unprivileged device node");
	if (unlink(ADMIN_FILE) || unlink(ADMIN_SPACE) ||
	    unlink(ADMIN_TMP_SPACE))
		return fail("cleanup");
	puts("FILE_ADMIN_RUNTIME_OK");
	return 0;
}
