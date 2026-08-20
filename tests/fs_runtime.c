#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define APPEND_RECORD_SIZE 16
#define APPEND_RECORDS 64
#define CREATE_VISIBILITY_ROUNDS 256

#define CREATE_VISIBILITY_FILE "/ext-create-visibility"
#define CREATE_VISIBILITY_DIR  "/ext-create-visibility-dir"
#define CREATE_VISIBILITY_NODE "/ext-create-visibility-node"

static char append_truncate_buffer[
	APPEND_RECORD_SIZE * (APPEND_RECORDS * 3 + 1)];

static int fail(int code)
{
	char message[64];
	int length;

	length = snprintf(message, sizeof(message),
	                  "FS_RUNTIME_FAIL=%d errno=%d\n", code, errno);
	write(1, message, length);
	return code;
}

static void pass(const char *name)
{
	write(1, name, strlen(name));
}

static int make_file(const char *path, const char *data)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	size_t length = strlen(data);

	if (fd < 0 || write(fd, data, length) != (ssize_t)length ||
	    fsync(fd) || close(fd))
		return -1;
	return 0;
}

static int directory_has(const char *path, const char *name)
{
	struct dirent *entry;
	DIR *directory = opendir(path);
	int found = 0;

	if (!directory)
		return -1;
	errno = 0;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, name)) {
			found = 1;
			break;
		}
	}
	if (errno || closedir(directory))
		return -1;
	return found;
}

struct create_visibility_state {
	int start;
	int stop;
	int failed;
};

static int transient_mode_visible(const char *path)
{
	struct stat state;

	if (!lstat(path, &state))
		return (state.st_mode & 0777) != 0;
	return errno == ENOENT ? 0 : -1;
}

static int test_ext4_creation_visibility(void)
{
	struct create_visibility_state *state;
	int fd, index, status, result = -1;
	pid_t child;

	unlink(CREATE_VISIBILITY_FILE);
	rmdir(CREATE_VISIBILITY_DIR);
	unlink(CREATE_VISIBILITY_NODE);
	state = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (state == MAP_FAILED)
		return -1;
	memset(state, 0, sizeof(*state));
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		while (!__atomic_load_n(&state->start, __ATOMIC_ACQUIRE))
			poll(NULL, 0, 1);
		while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) {
			if (transient_mode_visible(CREATE_VISIBILITY_FILE) ||
			    transient_mode_visible(CREATE_VISIBILITY_DIR) ||
			    transient_mode_visible(CREATE_VISIBILITY_NODE)) {
				__atomic_store_n(&state->failed, 1,
				                 __ATOMIC_RELEASE);
				break;
			}
		}
		_exit(0);
	}
	__atomic_store_n(&state->start, 1, __ATOMIC_RELEASE);
	for (index = 0; index < CREATE_VISIBILITY_ROUNDS; index++) {
		fd = open(CREATE_VISIBILITY_FILE,
		          O_CREAT | O_EXCL | O_WRONLY, 0000);
		if (fd < 0 || close(fd) || unlink(CREATE_VISIBILITY_FILE) ||
		    mkdir(CREATE_VISIBILITY_DIR, 0000) ||
		    rmdir(CREATE_VISIBILITY_DIR) ||
		    mknod(CREATE_VISIBILITY_NODE, S_IFCHR | 0000,
		          makedev(1, 3)) || unlink(CREATE_VISIBILITY_NODE)) {
			__atomic_store_n(&state->failed, 1,
			                 __ATOMIC_RELEASE);
			break;
		}
		if (__atomic_load_n(&state->failed, __ATOMIC_ACQUIRE))
			break;
	}
	__atomic_store_n(&state->stop, 1, __ATOMIC_RELEASE);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) ||
	    __atomic_load_n(&state->failed, __ATOMIC_ACQUIRE))
		goto out;
	result = 0;
out:
	unlink(CREATE_VISIBILITY_FILE);
	rmdir(CREATE_VISIBILITY_DIR);
	unlink(CREATE_VISIBILITY_NODE);
	munmap(state, 4096);
	return result;
}

struct test_linux_dirent64 {
	uint64_t ino;
	int64_t offset;
	uint16_t reclen;
	uint8_t type;
	char name[];
} __attribute__((packed));

static int dirent_buffer_has(const unsigned char *buffer, ssize_t length,
			     const char *name)
{
	const struct test_linux_dirent64 *entry;
	ssize_t offset = 0;

	while (offset < length) {
		entry = (const void *)(buffer + offset);
		if (length - offset < 24 || entry->reclen < 24 ||
		    entry->reclen > length - offset ||
		    !memchr(entry->name, 0, entry->reclen - 19))
			return -1;
		if (!strcmp(entry->name, name))
			return 1;
		offset += entry->reclen;
	}
	return offset == length ? 0 : -1;
}

static int test_getdents_boundary(const char *parent)
{
	static const char name[] = "entry-with-a-name-longer-than-four";
	unsigned char small[72], large[512];
	char directory[128], path[256];
	ssize_t length;
	int fd = -1, result = -1;

	snprintf(directory, sizeof(directory), "%s/getdents-boundary", parent);
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	unlink(path);
	rmdir(directory);
	if (mkdir(directory, 0700) || make_file(path, "x"))
		goto out;
	fd = open(directory, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		goto out;
	length = syscall(SYS_getdents64, fd, small, sizeof(small));
	if (length != 48 || dirent_buffer_has(small, length, name) != 0)
		goto out;
	length = syscall(SYS_getdents64, fd, large, sizeof(large));
	if (length <= 0 || dirent_buffer_has(large, length, name) != 1)
		goto out;
	result = 0;
out:
	if (fd >= 0 && close(fd))
		result = -1;
	if (unlink(path) || rmdir(directory))
		result = -1;
	return result;
}

static int append_writer(const char *path, const char *record)
{
	struct iovec iovecs[2] = {
		{ .iov_base = (void *)record, .iov_len = 8 },
		{ .iov_base = (void *)(record + 8), .iov_len = 8 },
	};
	int fd, index;

	poll(0, 0, 20);
	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0)
		return -1;
	for (index = 0; index < APPEND_RECORDS; index++) {
		if (writev(fd, iovecs, 2) != APPEND_RECORD_SIZE) {
			close(fd);
			return -1;
		}
	}
	return close(fd);
}

static int test_append_writev(const char *directory)
{
	static const char records[2][APPEND_RECORD_SIZE + 1] = {
		"A-BEGIN-A--END--",
		"B-BEGIN-B--END--",
	};
	char path[128];
	char buffer[APPEND_RECORD_SIZE * APPEND_RECORDS * 2];
	int counts[2] = { 0, 0 };
	pid_t children[2];
	int fd, index, offset, status;
	ssize_t length, total = 0;

	snprintf(path, sizeof(path), "%s/append", directory);
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0 || close(fd))
		return -1;
	for (index = 0; index < 2; index++) {
		children[index] = fork();
		if (children[index] < 0)
			return -1;
		if (!children[index])
			_exit(append_writer(path, records[index]) ? 1 : 0);
	}
	for (index = 0; index < 2; index++) {
		if (waitpid(children[index], &status, 0) != children[index] ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return -1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	while (total < (ssize_t)sizeof(buffer)) {
		length = read(fd, buffer + total, sizeof(buffer) - total);
		if (length <= 0) {
			close(fd);
			return -1;
		}
		total += length;
	}
	if (close(fd))
		return -1;
	for (offset = 0; offset < (int)sizeof(buffer);
	     offset += APPEND_RECORD_SIZE) {
		if (!memcmp(buffer + offset, records[0], APPEND_RECORD_SIZE))
			counts[0]++;
		else if (!memcmp(buffer + offset, records[1],
				 APPEND_RECORD_SIZE))
			counts[1]++;
		else
			return -1;
	}
	return counts[0] == APPEND_RECORDS &&
		counts[1] == APPEND_RECORDS ? 0 : -1;
}

static int truncate_writer(const char *path, const char *record)
{
	struct iovec iovecs[2] = {
		{ .iov_base = (void *)record, .iov_len = 8 },
		{ .iov_base = (void *)(record + 8), .iov_len = 8 },
	};
	int fd, index;

	poll(0, 0, 20);
	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0)
		return -1;
	for (index = 0; index < APPEND_RECORDS; index++) {
		if (ftruncate(fd, 0) ||
		    writev(fd, iovecs, 2) != APPEND_RECORD_SIZE) {
			close(fd);
			return -1;
		}
	}
	return close(fd);
}

static int test_append_truncate(const char *directory)
{
	static const char records[4][APPEND_RECORD_SIZE + 1] = {
		"A-BEGIN-A--END--",
		"B-BEGIN-B--END--",
		"T-BEGIN-T--END--",
		"P-BEGIN-P--END--",
	};
	struct iovec iovecs[2] = {
		{ .iov_base = (void *)records[3], .iov_len = 8 },
		{ .iov_base = (void *)(records[3] + 8), .iov_len = 8 },
	};
	char path[128];
	struct stat statbuf;
	pid_t children[3];
	ssize_t length, total = 0;
	int child, fd, offset, record, status;

	snprintf(path, sizeof(path), "%s/append-truncate", directory);
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0 || close(fd))
		return -1;
	for (child = 0; child < 3; child++) {
		children[child] = fork();
		if (children[child] < 0)
			return -1;
		if (!children[child]) {
			int result = child < 2 ?
				append_writer(path, records[child]) :
				truncate_writer(path, records[2]);

			_exit(result ? 1 : 0);
		}
	}
	for (child = 0; child < 3; child++) {
		if (waitpid(children[child], &status, 0) != children[child] ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return -1;
	}
	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0 || writev(fd, iovecs, 2) != APPEND_RECORD_SIZE ||
	    close(fd))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &statbuf) || !statbuf.st_size ||
	    statbuf.st_size > (off_t)sizeof(append_truncate_buffer) ||
	    statbuf.st_size % APPEND_RECORD_SIZE)
		return -1;
	while (total < (ssize_t)statbuf.st_size) {
		length = read(fd, append_truncate_buffer + total,
			      statbuf.st_size - total);
		if (length <= 0) {
			close(fd);
			return -1;
		}
		total += length;
	}
	if (close(fd) ||
	    memcmp(append_truncate_buffer + total - APPEND_RECORD_SIZE,
		   records[3], APPEND_RECORD_SIZE))
		return -1;
	for (offset = 0; offset < total; offset += APPEND_RECORD_SIZE) {
		for (record = 0; record < 4; record++) {
			if (!memcmp(append_truncate_buffer + offset,
				    records[record], APPEND_RECORD_SIZE))
				break;
		}
		if (record == 4)
			return -1;
	}
	return 0;
}

static int test_devices(void)
{
	unsigned char buffer[8192];
	struct pollfd pollfds[2];
	struct stat statbuf;
	int fd, i, null_fd;

	fd = open("/dev/zero", O_RDONLY);
	if (fd < 0 || read(fd, buffer, sizeof(buffer)) != sizeof(buffer))
		return 10;
	for (i = 0; i < (int)sizeof(buffer); i++) {
		if (buffer[i])
			return 11;
	}
	null_fd = open("/dev/null", O_RDWR);
	if (null_fd < 0 ||
	    write(null_fd, buffer, sizeof(buffer)) != sizeof(buffer) ||
	    read(null_fd, buffer, sizeof(buffer)) != 0)
		return 13;
	pollfds[0].fd = fd;
	pollfds[0].events = POLLIN | POLLOUT;
	pollfds[1].fd = null_fd;
	pollfds[1].events = POLLIN | POLLOUT;
	if (poll(pollfds, 2, 0) != 2 ||
	    pollfds[0].revents != (POLLIN | POLLOUT) ||
	    pollfds[1].revents != (POLLIN | POLLOUT))
		return 15;
	if (close(null_fd) || close(fd))
		return 12;
	if (stat("/dev/console", &statbuf) || !S_ISCHR(statbuf.st_mode) ||
	    major(statbuf.st_rdev) != 5 || minor(statbuf.st_rdev) != 1)
		return 14;
	return 0;
}

static int test_tree(const char *directory)
{
	char source[128], hard[128], symbolic[128], target[128];
	char replacement[128], sparse[128], child[128], renamed[128];
	char buffer[16];
	struct stat statbuf;
	int fd, i, oldfd;

	snprintf(source, sizeof(source), "%s/source", directory);
	snprintf(hard, sizeof(hard), "%s/hard", directory);
	snprintf(symbolic, sizeof(symbolic), "%s/symbolic", directory);
	snprintf(target, sizeof(target), "%s/target", directory);
	snprintf(replacement, sizeof(replacement),
	         "%s/replacement", directory);
	snprintf(sparse, sizeof(sparse), "%s/sparse", directory);
	snprintf(child, sizeof(child), "%s/child", directory);
	snprintf(renamed, sizeof(renamed), "%s/renamed", directory);
	if (mkdir(directory, 0755) && errno != EEXIST)
		return 20;
	if (make_file(source, "payload") || link(source, hard) ||
	    symlink("source", symbolic))
		return 21;
	memset(buffer, 0, sizeof(buffer));
	if (readlink(symbolic, buffer, sizeof(buffer)) != 6 ||
	    memcmp(buffer, "source", 6))
		return 22;
	if (lstat(symbolic, &statbuf) || !S_ISLNK(statbuf.st_mode) ||
	    statbuf.st_size != 6)
		return 37;
	fd = open(symbolic, O_RDONLY);
	if (fd < 0 || read(fd, buffer, 7) != 7 ||
	    memcmp(buffer, "payload", 7) || close(fd))
		return 23;
	if (stat(source, &statbuf) || statbuf.st_nlink != 2)
		return 24;

	fd = open(source, O_RDWR);
	if (fd < 0 || unlink(source) || lseek(fd, 0, SEEK_SET) != 0 ||
	    read(fd, buffer, 7) != 7 || memcmp(buffer, "payload", 7))
		return 25;
	if (!stat(source, &statbuf) || errno != ENOENT || close(fd))
		return 26;

	if (make_file(target, "old") || make_file(replacement, "new"))
		return 27;
	oldfd = open(target, O_RDONLY);
	if (oldfd < 0 || rename(replacement, target) ||
	    read(oldfd, buffer, 3) != 3 || memcmp(buffer, "old", 3) ||
	    close(oldfd))
		return 28;
	fd = open(target, O_RDONLY);
	if (fd < 0 || read(fd, buffer, 3) != 3 ||
	    memcmp(buffer, "new", 3) || close(fd))
		return 29;

	fd = open(sparse, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0 || lseek(fd, 8192, SEEK_SET) != 8192 ||
	    write(fd, "Z", 1) != 1 || fstat(fd, &statbuf) ||
	    statbuf.st_size != 8193 || statbuf.st_blocks > 16 ||
	    lseek(fd, 4096, SEEK_SET) != 4096 ||
	    read(fd, buffer, sizeof(buffer)) != sizeof(buffer))
		return 30;
	for (i = 0; i < (int)sizeof(buffer); i++) {
		if (buffer[i])
			return 31;
	}
	if (lseek(fd, 8192, SEEK_SET) != 8192 ||
	    read(fd, buffer, 1) != 1 || buffer[0] != 'Z')
		return 32;
	errno = 0;
	if (ftruncate(fd, -1) != -1 || errno != EINVAL ||
	    fstat(fd, &statbuf) || statbuf.st_size != 8193)
		return 38;
	if (ftruncate(fd, 12289) || fstat(fd, &statbuf) ||
	    statbuf.st_size != 12289 ||
	    lseek(fd, 8192, SEEK_SET) != 8192 ||
	    read(fd, buffer, sizeof(buffer)) != sizeof(buffer) ||
	    buffer[0] != 'Z')
		return 42;
	for (i = 1; i < (int)sizeof(buffer); i++) {
		if (buffer[i])
			return 43;
	}
	if (lseek(fd, 12273, SEEK_SET) != 12273 ||
	    read(fd, buffer, sizeof(buffer)) != sizeof(buffer))
		return 44;
	for (i = 0; i < (int)sizeof(buffer); i++) {
		if (buffer[i])
			return 45;
	}
	if (close(fd))
		return 33;
	fd = open(sparse, O_RDONLY);
	errno = 0;
	if (fd < 0 || ftruncate(fd, 0) != -1 || errno != EBADF ||
	    close(fd) || stat(sparse, &statbuf) ||
	    statbuf.st_size != 12289)
		return 39;
	fd = open(sparse, O_WRONLY | O_TRUNC);
	if (fd < 0 || close(fd) || stat(sparse, &statbuf) || statbuf.st_size)
		return 34;
	if (directory_has(directory, "target") != 1 ||
	    directory_has(directory, "missing") != 0)
		return 35;
	if (test_append_writev(directory))
		return 40;
	if (test_append_truncate(directory))
		return 41;
	if (mkdir(child, 0755) || rename(child, renamed) || rmdir(renamed))
		return 36;
	return 0;
}

static int test_fat(int *mounted)
{
	static const char *long_name =
		"/mnt/fat/runtime/long-咖啡-file-name.txt";
	char buffer[16];
	struct stat mount_stat, root_stat;
	int fd;

	*mounted = 0;
	if (stat("/", &root_stat) || stat("/mnt/fat", &mount_stat))
		return 220;
	if (root_stat.st_dev == mount_stat.st_dev)
		return 0;
	*mounted = 1;
	if (mkdir("/mnt/fat/runtime", 0755))
		return 221;
	if (make_file("/mnt/fat/runtime/source", "fat-data"))
		return 222;
	fd = open("/mnt/fat/runtime/source", O_RDWR);
	if (fd < 0)
		return 223;
	errno = 0;
	if (!unlink("/mnt/fat/runtime/source") || errno != EBUSY ||
	    lseek(fd, 0, SEEK_SET) != 0 || read(fd, buffer, 8) != 8 ||
	    memcmp(buffer, "fat-data", 8) || close(fd))
		return 224;
	if (unlink("/mnt/fat/runtime/source"))
		return 225;
	if (make_file("/mnt/fat/runtime/link-source", "link"))
		return 226;
	errno = 0;
	if (!link("/mnt/fat/runtime/link-source",
	          "/mnt/fat/runtime/hard") || errno != EOPNOTSUPP)
		return 227;
	errno = 0;
	if (!symlink("link-source", "/mnt/fat/runtime/symbolic") ||
	    errno != EOPNOTSUPP)
		return 228;
	if (make_file("/mnt/fat/runtime/target", "old") ||
	    make_file("/mnt/fat/runtime/replacement", "new") ||
	    rename("/mnt/fat/runtime/replacement",
	           "/mnt/fat/runtime/target"))
		return 229;
	fd = open("/mnt/fat/runtime/target", O_RDONLY);
	if (fd < 0 || read(fd, buffer, 3) != 3 ||
	    memcmp(buffer, "new", 3) || close(fd))
		return 230;
	if (make_file(long_name, "utf8"))
		return 231;
	fd = open(long_name, O_RDONLY);
	if (fd < 0 || read(fd, buffer, 4) != 4 ||
	    memcmp(buffer, "utf8", 4) || close(fd))
		return 232;
	return 0;
}

int main(void)
{
	int result = test_devices();
	int fat_mounted;

	if (result)
		return fail(result);
	pass("DEVFS_OK\n");
	result = test_tree("/ext-runtime");
	if (result)
		return fail(result);
	if (test_ext4_creation_visibility())
		return fail(239);
	if (test_getdents_boundary("/ext-runtime"))
		return fail(233);
	pass("EXT4_OK\n");
	result = test_tree("/tmp/tmp-runtime");
	if (result)
		return fail(result + 100);
	if (test_getdents_boundary("/tmp/tmp-runtime"))
		return fail(234);
	pass("TMPFS_OK\n");
	result = test_fat(&fat_mounted);
	if (result)
		return fail(result);
	if (fat_mounted && test_getdents_boundary("/mnt/fat/runtime"))
		return fail(235);
	pass(fat_mounted ? "FAT_OK\n" : "FAT_SKIP\n");
	write(1, "FS_RUNTIME_OK\n", 14);
	return 0;
}
