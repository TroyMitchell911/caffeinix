#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif

#define VECTORED_HIGH_OFFSET ((off_t)(1ULL << 32) + 127)
#define READV_RECORD_HALF 64
#define READV_RECORDS 256
#define READV_REF_LOOPS 128

struct readv_worker {
	atomic_int *start;
	int fd;
	int records;
	int failed;
};
static int fail(const char *operation)
{
	printf("IO_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	return 1;
}

static int read_exact(int fd, char *buffer, size_t length, off_t offset)
{
	ssize_t result = pread(fd, buffer, length, offset);

	return result == (ssize_t)length ? 0 : -1;
}

static void *readv_worker(void *argument)
{
	struct readv_worker *worker = argument;
	unsigned char first[READV_RECORD_HALF];
	unsigned char second[READV_RECORD_HALF];
	struct iovec iovecs[2] = {
		{ .iov_base = first, .iov_len = sizeof(first) },
		{ .iov_base = second, .iov_len = sizeof(second) },
	};
	ssize_t result;
	size_t index;

	while (!atomic_load_explicit(worker->start, memory_order_acquire))
		usleep(1000);
	for (;;) {
		result = readv(worker->fd, iovecs, 2);
		if (!result)
			return NULL;
		if (result != (ssize_t)(sizeof(first) + sizeof(second)) ||
		    first[0] != second[0])
			break;
		for (index = 1; index < sizeof(first); index++) {
			if (first[index] != first[0] || second[index] != first[0])
				goto failed;
		}
		worker->records++;
	}
failed:
	worker->failed = 1;
	return NULL;
}

static int test_readv_shared_position(void)
{
	struct readv_worker workers[2];
	unsigned char record[2 * READV_RECORD_HALF];
	atomic_int start;
	pthread_t threads[2];
	int created = 0, failed = 1, fd = -1, index;

	fd = open("/tmp/io-readv-position", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		goto out;
	for (index = 0; index < READV_RECORDS; index++) {
		memset(record, index % 251 + 1, sizeof(record));
		if (write(fd, record, sizeof(record)) != sizeof(record))
			goto out;
	}
	if (lseek(fd, 0, SEEK_SET) != 0)
		goto out;
	atomic_init(&start, 0);
	memset(workers, 0, sizeof(workers));
	for (index = 0; index < 2; index++) {
		workers[index].start = &start;
		workers[index].fd = fd;
		if (pthread_create(&threads[index], NULL, readv_worker,
				   &workers[index]))
			goto join;
		created++;
	}
	atomic_store_explicit(&start, 1, memory_order_release);
	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
	created = 0;
	if (workers[0].failed || workers[1].failed ||
	    workers[0].records + workers[1].records != READV_RECORDS)
		goto out;
	failed = 0;
	goto out;
join:
	atomic_store_explicit(&start, 1, memory_order_release);
	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
out:
	if (fd >= 0)
		close(fd);
	unlink("/tmp/io-readv-position");
	return failed ? -1 : 0;
}

static int test_readv_file_references(void)
{
	struct iovec iovec = { 0 };
	int fd, index;

	for (index = 0; index < READV_REF_LOOPS; index++) {
		fd = open("/tmp/io-source", O_RDONLY);
		if (fd < 0 || readv(fd, &iovec, 1) != 0 || close(fd)) {
			if (fd >= 0)
				close(fd);
			return -1;
		}
	}
	return 0;
}

static int test_append_positioned_writes(void)
{
	const char first[] = "X";
	const char second[] = "YZ";
	const char expected[] = "aXYZ123";
	struct iovec iovecs[2] = {
		{ .iov_base = (void *)first, .iov_len = sizeof(first) - 1 },
		{ .iov_base = (void *)second, .iov_len = sizeof(second) - 1 },
	};
	char contents[sizeof(expected) - 1];
	int fd;

	fd = open("/tmp/io-append", O_CREAT | O_TRUNC | O_RDWR | O_APPEND,
		  0600);
	if (fd < 0 || write(fd, "abc", 3) != 3 ||
	    syscall(SYS_pwrite64, fd, "D", 1, 0) != 1 ||
	    syscall(SYS_pwritev, fd, iovecs, 2, 0L, 0L) != 3 ||
	    lseek(fd, 0, SEEK_CUR) != 3 ||
	    syscall(SYS_pwritev2, fd, iovecs, 2, 1L, 0L,
		    RWF_NOAPPEND) != 3 ||
	    lseek(fd, 4, SEEK_SET) != 4) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	iovecs[0].iov_base = (void *)"1";
	iovecs[1].iov_base = (void *)"23";
	if (syscall(SYS_pwritev2, fd, iovecs, 2, -1L, -1L,
		    RWF_NOAPPEND) != 3 ||
	    lseek(fd, 0, SEEK_CUR) != 7 ||
	    read_exact(fd, contents, sizeof(contents), 0) < 0 ||
	    memcmp(contents, expected, sizeof(contents)) || close(fd) ||
	    unlink("/tmp/io-append"))
		return -1;
	return 0;
}

static int test_high_positioned_vectors(void)
{
	const char first = 'H', second = 'I';
	struct iovec write_iovecs[2] = {
		{ .iov_base = (void *)&first, .iov_len = 1 },
		{ .iov_base = (void *)&second, .iov_len = 1 },
	};
	char contents[2] = { 0 };
	struct iovec read_iovec = {
		.iov_base = contents,
		.iov_len = sizeof(contents),
	};
	struct stat stat_buffer;
	uint32_t low = (uint32_t)VECTORED_HIGH_OFFSET;
	uint32_t high = (uint64_t)VECTORED_HIGH_OFFSET >> 32;
	int fd;

	fd = open("/tmp/io-high-vector", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 ||
	    syscall(SYS_pwritev, fd, write_iovecs, 1, low, high) != 1 ||
	    syscall(SYS_pwritev2, fd, &write_iovecs[1], 1, low + 1,
		    high, 0) != 1 ||
	    syscall(SYS_preadv, fd, &read_iovec, 1, low, high) != 2 ||
	    memcmp(contents, "HI", sizeof(contents)) ||
	    syscall(SYS_preadv2, fd, &read_iovec, 1, low, high, 0) != 2 ||
	    memcmp(contents, "HI", sizeof(contents)) ||
	    fstat(fd, &stat_buffer) ||
	    stat_buffer.st_size != VECTORED_HIGH_OFFSET + 2 ||
	    close(fd) || unlink("/tmp/io-high-vector")) {
		if (fd >= 0)
			close(fd);
		unlink("/tmp/io-high-vector");
		return -1;
	}
	return 0;
}

static int test_positioned_offset_limit(void)
{
	char bytes[2] = { 'x', 'y' };
	struct iovec iovec = {
		.iov_base = bytes,
		.iov_len = sizeof(bytes),
	};
	struct stat status;
	int fd;

	fd = open("/tmp/io-position-limit", O_CREAT | O_EXCL | O_RDWR,
	          0600);
	if (fd < 0)
		return -1;
	errno = 0;
	if (pwrite(fd, bytes, 1, INT64_MAX) != -1 || errno != EINVAL)
		goto fail;
	errno = 0;
	if (pwritev(fd, &iovec, 1, INT64_MAX - 1) != -1 ||
	    errno != EINVAL)
		goto fail;
	errno = 0;
	if (pwritev2(fd, &iovec, 1, INT64_MAX - 1, 0) != -1 ||
	    errno != EINVAL)
		goto fail;
	if (fstat(fd, &status) || status.st_size || close(fd) ||
	    unlink("/tmp/io-position-limit"))
		return -1;
	return 0;

fail:
	close(fd);
	unlink("/tmp/io-position-limit");
	return -1;
}

static int test_write_only_positioned_reads(void)
{
	char buffer;
	struct iovec iovec = {
		.iov_base = &buffer,
		.iov_len = sizeof(buffer),
	};
	int fd = open("/tmp/io-source", O_WRONLY);

	if (fd < 0)
		return -1;
	errno = 0;
	if (pread(fd, &buffer, sizeof(buffer), 0) != -1 || errno != EBADF)
		goto fail;
	errno = 0;
	if (preadv(fd, &iovec, 1, 0) != -1 || errno != EBADF)
		goto fail;
	errno = 0;
	if (preadv2(fd, &iovec, 1, 0, 0) != -1 || errno != EBADF)
		goto fail;
	return close(fd);
fail:
	close(fd);
	return -1;
}

int main(void)
{
	const char source[] = "abcdefghijklmnopqrstuvwxyz";
	const char write_first[] = "12";
	const char write_second[] = "345";
	struct iovec iov[2], write_iov[2];
	char first[3], second[4], result[14] = { 0 };
	int input, readonly;

	input = open("/tmp/io-source", O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (input < 0 || write(input, source, sizeof(source) - 1) !=
	    (ssize_t)(sizeof(source) - 1))
		return fail("fixture");
	if (lseek(input, 2, SEEK_SET) != 2)
		return fail("readv seek");
	iov[0].iov_base = first;
	iov[0].iov_len = sizeof(first);
	iov[1].iov_base = second;
	iov[1].iov_len = sizeof(second);
	if (readv(input, iov, 2) != 7 ||
	    memcmp(first, "cde", sizeof(first)) ||
	    memcmp(second, "fghi", sizeof(second)) ||
	    lseek(input, 0, SEEK_CUR) != 9)
		return fail("readv");
	if (test_readv_file_references())
		return fail("readv file references");
	if (test_readv_shared_position())
		return fail("readv shared position");
	if (pwrite(input, "XY", 2, 5) != 2 ||
	    lseek(input, 0, SEEK_CUR) != 9 ||
	    read_exact(input, result, 8, 0) < 0 ||
	    memcmp(result, "abcdeXYh", 8))
		return fail("pwrite");

	iov[0].iov_base = first;
	iov[0].iov_len = 2;
	iov[1].iov_base = second;
	iov[1].iov_len = 3;
	if (preadv(input, iov, 2, 4) != 5 ||
	    memcmp(first, "eX", 2) || memcmp(second, "Yhi", 3) ||
	    lseek(input, 0, SEEK_CUR) != 9)
		return fail("preadv");
	write_iov[0].iov_base = (void *)write_first;
	write_iov[0].iov_len = sizeof(write_first) - 1;
	write_iov[1].iov_base = (void *)write_second;
	write_iov[1].iov_len = sizeof(write_second) - 1;
	if (pwritev(input, write_iov, 2, 8) != 5 ||
	    lseek(input, 0, SEEK_CUR) != 9 ||
	    read_exact(input, result, 13, 0) < 0 ||
	    memcmp(result, "abcdeXYh12345", 13))
		return fail("pwritev");
	if (lseek(input, 13, SEEK_SET) != 13 ||
	    syscall(SYS_preadv2, input, iov, 2, -1L, -1L, 0) != 5 ||
	    memcmp(first, "no", 2) || memcmp(second, "pqr", 3) ||
	    lseek(input, 0, SEEK_CUR) != 18)
		return fail("preadv2 current offset");
	errno = 0;
	if (pwritev2(input, write_iov, 2, 0, RWF_HIPRI) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("pwritev2 flags");
	if (test_append_positioned_writes())
		return fail("positioned append");
	if (test_high_positioned_vectors())
		return fail("high positioned vectors");
	if (test_positioned_offset_limit())
		return fail("positioned offset limit");
	if (test_write_only_positioned_reads())
		return fail("writeonly positioned reads");
	readonly = open("/tmp/io-source", O_RDONLY);
	if (readonly < 0)
		return fail("readonly open");
	errno = 0;
	if (pwrite(readonly, result, 1, 0) != -1 || errno != EBADF)
		return fail("readonly pwrite");
	errno = 0;
	if (pwritev(readonly, write_iov, 2, 0) != -1 || errno != EBADF)
		return fail("readonly pwritev");
	errno = 0;
	if (pwritev2(readonly, write_iov, 2, 0, 0) != -1 ||
	    errno != EBADF)
		return fail("readonly pwritev2");
	if (close(readonly))
		return fail("readonly close");

	errno = 0;
	if (syscall(SYS_readv, input, iov, 1025) != -1 || errno != EINVAL)
		return fail("readv limit");
	errno = 0;
	if (pwrite(input, result, 1, -1) != -1 || errno != EINVAL)
		return fail("pwrite offset");
	if (close(input) < 0)
		return fail("close");

	puts("VECTORED_IO_OK");
	return 0;
}
