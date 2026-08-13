#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

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
	pass("EXT4_OK\n");
	result = test_tree("/tmp/tmp-runtime");
	if (result)
		return fail(result + 100);
	pass("TMPFS_OK\n");
	result = test_fat(&fat_mounted);
	if (result)
		return fail(result);
	pass(fat_mounted ? "FAT_OK\n" : "FAT_SKIP\n");
	write(1, "FS_RUNTIME_OK\n", 14);
	return 0;
}
