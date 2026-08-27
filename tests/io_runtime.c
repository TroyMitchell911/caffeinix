#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif

#define VECTORED_HIGH_OFFSET ((off_t)(1ULL << 32) + 127)
#define SENDFILE_CONCURRENT_CHUNK (32 * 1024)
#define READV_RECORD_HALF 64
#define READV_RECORDS 256
#define READV_REF_LOOPS 128

struct sendfile_worker {
	atomic_int *start;
	int input;
	int output;
	ssize_t result;
};

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

static int compare_files(int first, int second, size_t length)
{
	char first_buffer[512], second_buffer[512];
	size_t chunk, offset = 0;

	while (offset < length) {
		chunk = length - offset;
		if (chunk > sizeof(first_buffer))
			chunk = sizeof(first_buffer);
		if (read_exact(first, first_buffer, chunk, offset) < 0 ||
		    read_exact(second, second_buffer, chunk, offset) < 0 ||
		    memcmp(first_buffer, second_buffer, chunk))
			return -1;
		offset += chunk;
	}
	return 0;
}

static int file_contains_byte(int fd, unsigned char value, size_t length)
{
	unsigned char buffer[512];
	size_t chunk, index, offset = 0;

	while (offset < length) {
		chunk = length - offset;
		if (chunk > sizeof(buffer))
			chunk = sizeof(buffer);
		if (pread(fd, buffer, chunk, offset) != (ssize_t)chunk)
			return -1;
		for (index = 0; index < chunk; index++) {
			if (buffer[index] != value)
				return -1;
		}
		offset += chunk;
	}
	return 0;
}

static void *sendfile_worker(void *argument)
{
	struct sendfile_worker *worker = argument;

	while (!atomic_load_explicit(worker->start, memory_order_acquire))
		usleep(1000);
	worker->result = sendfile(worker->output, worker->input, NULL,
				  SENDFILE_CONCURRENT_CHUNK);
	return NULL;
}

static int test_concurrent_sendfile(void)
{
	static const char *const paths[] = {
		"/tmp/io-sendfile-shared-source",
		"/tmp/io-sendfile-shared-first",
		"/tmp/io-sendfile-shared-second",
	};
	struct sendfile_worker workers[2];
	unsigned char buffer[512], values[2];
	atomic_int start;
	pthread_t threads[2];
	off_t position;
	int created = 0, failed = 1, index, input = -1;

	workers[0].output = -1;
	workers[1].output = -1;
	input = open(paths[0], O_CREAT | O_EXCL | O_RDWR, 0600);
	workers[0].output = open(paths[1], O_CREAT | O_EXCL | O_RDWR, 0600);
	workers[1].output = open(paths[2], O_CREAT | O_EXCL | O_RDWR, 0600);
	if (input < 0 || workers[0].output < 0 || workers[1].output < 0)
		goto out;
	for (index = 0; index < 2 * SENDFILE_CONCURRENT_CHUNK /
				       (int)sizeof(buffer); index++) {
		memset(buffer, index < SENDFILE_CONCURRENT_CHUNK /
					 (int)sizeof(buffer) ? 'A' : 'B',
		       sizeof(buffer));
		if (write(input, buffer, sizeof(buffer)) != sizeof(buffer))
			goto out;
	}
	if (lseek(input, 0, SEEK_SET) != 0)
		goto out;
	atomic_init(&start, 0);
	workers[0].start = &start;
	workers[0].input = input;
	workers[0].result = -1;
	workers[1].start = &start;
	workers[1].input = input;
	workers[1].result = -1;
	if (pthread_create(&threads[0], NULL, sendfile_worker, &workers[0]))
		goto out;
	created = 1;
	if (pthread_create(&threads[1], NULL, sendfile_worker, &workers[1]))
		goto join;
	created = 2;
	atomic_store_explicit(&start, 1, memory_order_release);
	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
	created = 0;
	position = lseek(input, 0, SEEK_CUR);
	if (workers[0].result != SENDFILE_CONCURRENT_CHUNK ||
	    workers[1].result != SENDFILE_CONCURRENT_CHUNK ||
	    position != 2 * SENDFILE_CONCURRENT_CHUNK ||
	    pread(workers[0].output, &values[0], 1, 0) != 1 ||
	    pread(workers[1].output, &values[1], 1, 0) != 1 ||
	    values[0] == values[1] ||
	    (values[0] != 'A' && values[0] != 'B') ||
	    (values[1] != 'A' && values[1] != 'B') ||
	    file_contains_byte(workers[0].output, values[0],
			       SENDFILE_CONCURRENT_CHUNK) ||
	    file_contains_byte(workers[1].output, values[1],
			       SENDFILE_CONCURRENT_CHUNK))
		goto out;
	failed = 0;
	goto out;

join:
	atomic_store_explicit(&start, 1, memory_order_release);
	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
out:
	if (workers[0].output >= 0)
		close(workers[0].output);
	if (workers[1].output >= 0)
		close(workers[1].output);
	if (input >= 0)
		close(input);
	for (index = 0; index < 3; index++)
		unlink(paths[index]);
	return failed ? -1 : 0;
}

static int test_sendfile_read_access(void)
{
	struct stat state;
	int input = -1, output = -1, result = -1;

	input = open("/tmp/io-source", O_WRONLY);
	output = open("/tmp/io-sendfile-writeonly",
	              O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (input < 0 || output < 0)
		goto out;
	errno = 0;
	if (sendfile(output, input, NULL, 1) != -1 || errno != EBADF ||
	    fstat(output, &state) || state.st_size)
		goto out;
	result = 0;
out:
	if (output >= 0)
		close(output);
	if (input >= 0)
		close(input);
	unlink("/tmp/io-sendfile-writeonly");
	return result;
}

int main(void)
{
	const char source[] = "abcdefghijklmnopqrstuvwxyz";
	const char write_first[] = "12";
	const char write_second[] = "345";
	struct iovec iov[2], write_iov[2];
	char block[1024];
	char first[3], second[4], result[14] = { 0 };
	off_t offset;
	int i, input, output, readonly;

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
	if (test_sendfile_read_access())
		return fail("sendfile read access");

	output = open("/tmp/io-sendfile", O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (output < 0 || lseek(input, 3, SEEK_SET) != 3)
		return fail("sendfile setup");
	offset = 1;
	if (sendfile(output, input, &offset, 5) != 5 || offset != 6 ||
	    lseek(input, 0, SEEK_CUR) != 3 ||
	    read_exact(output, result, 5, 0) < 0 ||
	    memcmp(result, "bcdeX", 5))
		return fail("sendfile offset");
	if (close(output) < 0)
		return fail("sendfile close");

	output = open("/tmp/io-sendfile-current",
		      O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (output < 0 || lseek(input, 0, SEEK_SET) != 0 ||
	    sendfile(output, input, NULL, 4) != 4 ||
	    lseek(input, 0, SEEK_CUR) != 4 ||
	    read_exact(output, result, 4, 0) < 0 ||
	    memcmp(result, "abcd", 4))
		return fail("sendfile current");
	if (close(output) < 0)
		return fail("sendfile current close");
	output = open("/tmp/io-sendfile-append",
		      O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0600);
	if (output < 0)
		return fail("sendfile append setup");
	errno = 0;
	if (sendfile(output, input, NULL, 1) != -1 || errno != EINVAL ||
	    lseek(input, 0, SEEK_CUR) != 4) {
		close(output);
		unlink("/tmp/io-sendfile-append");
		return fail("sendfile append rejection");
	}
	if (close(output) < 0 || unlink("/tmp/io-sendfile-append") < 0 ||
	    lseek(input, 0, SEEK_END) < 0)
		return fail("sendfile bulk setup");
	for (i = 0; i < (int)sizeof(block); i++)
		block[i] = i * 31 + 7;
	for (i = 0; i < 9; i++) {
		if (write(input, block, sizeof(block)) !=
		    (ssize_t)sizeof(block))
			return fail("sendfile bulk fixture");
	}
	output = open("/tmp/io-sendfile-bulk",
		      O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (output < 0 || lseek(input, 0, SEEK_SET) != 0 ||
	    sendfile(output, input, NULL, 8209) != 8209 ||
	    lseek(input, 0, SEEK_CUR) != 8209 ||
	    compare_files(input, output, 8209) < 0)
		return fail("sendfile bulk");

	errno = 0;
	if (syscall(SYS_readv, input, iov, 1025) != -1 || errno != EINVAL)
		return fail("readv limit");
	errno = 0;
	if (pwrite(input, result, 1, -1) != -1 || errno != EINVAL)
		return fail("pwrite offset");
	if (close(output) < 0 || close(input) < 0)
		return fail("close");
	if (test_concurrent_sendfile())
		return fail("concurrent sendfile");

	puts("VECTORED_IO_OK");
	puts("SENDFILE_OK");
	return 0;
}
