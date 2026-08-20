#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MOUNT_POINT "/tmp/mount-runtime"
#define PROC_POINT  "/tmp/mount-proc"
#define FAT_POINT   "/mnt/fat"
#define FAT_RACE_A  "/tmp/mount-fat-a"
#define FAT_RACE_B  "/tmp/mount-fat-b"
#define RELATIVE_BASE "/tmp/mount-relative"
#define RELATIVE_MOVED "/tmp/mount-relative-moved"
#define ATIME_RACE_THREADS 8
#define ATIME_RACE_READS   512
#define DIRECTORY_REBIND_ENTRIES 128
#define DIRECTORY_REBIND_TRIALS  16
#define DIRECTORY_REBIND_BUFFER  8192

static const char *failure_detail;
static char fat_device[32];

struct mount_worker {
	const char *target;
	atomic_int *start;
	atomic_int *abort;
	int result;
	int error;
};

struct sync_worker {
	atomic_int *start;
	atomic_int *stop;
};

struct atime_worker {
	const char *path;
	atomic_int *ready;
	atomic_int *start;
	atomic_int *done;
	int result;
};

struct directory_rebind_worker {
	int first_fd;
	int second_fd;
	int target_fd;
	atomic_int ready;
	atomic_int start;
	atomic_int stop;
	int result;
};

static char directory_rebind_buffer[DIRECTORY_REBIND_BUFFER];

static int fail(const char *step)
{
	printf("MOUNT_RUNTIME_FAIL %s errno=%d\n", step, errno);
	return 1;
}

static int expect_error(int result, int expected)
{
	return result == -1 && errno == expected ? 0 : -1;
}

static int wait_success(pid_t child)
{
	int status;

	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int find_fat_device(void)
{
	struct stat device_stat, mount_stat;
	int length;

	if (stat(FAT_POINT, &mount_stat) || !mount_stat.st_dev)
		return -1;
	length = snprintf(fat_device, sizeof(fat_device),
			  "/dev/virtio-blk%llu",
			  (unsigned long long)mount_stat.st_dev - 1);
	if (length < 0 || (size_t)length >= sizeof(fat_device) ||
	    stat(fat_device, &device_stat) || !S_ISBLK(device_stat.st_mode))
		return -1;
	return 0;
}

static int check_permission(void)
{
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (!child) {
		if (setresgid(1001, 1001, 1001) ||
		    setresuid(1001, 1001, 1001))
			_exit(1);
		errno = 0;
		if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL) != -1 ||
		    errno != EPERM)
			_exit(1);
		errno = 0;
		if (umount2(FAT_POINT, 0) != -1 || errno != EPERM)
			_exit(1);
		_exit(0);
	}
	return wait_success(child);
}

static int check_open_busy(void)
{
	int fd;

	if (mount(NULL, MOUNT_POINT, "tmpfs", 0, NULL))
		return -1;
	fd = open(MOUNT_POINT "/held", O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return -1;
	errno = 0;
	if (expect_error(umount2(MOUNT_POINT, 0), EBUSY)) {
		close(fd);
		return -1;
	}
	if (close(fd) || umount2(MOUNT_POINT, 0))
		return -1;
	return 0;
}

static int check_proc_mount(void)
{
	int fd;

	if (mkdir(PROC_POINT, 0700) ||
	    mount("proc", PROC_POINT, "proc", 0, NULL))
		return -1;
	fd = open(PROC_POINT "/self/status", O_RDONLY);
	if (fd < 0 || close(fd) || umount2(PROC_POINT, 0) ||
	    rmdir(PROC_POINT))
		return -1;
	return 0;
}

static int read_mount_snapshot(char *buffer, size_t size)
{
	ssize_t count;
	int fd;

	fd = open("/proc/mounts", O_RDONLY);
	if (fd < 0)
		return -1;
	count = read(fd, buffer, size - 1);
	if (count <= 0 || close(fd))
		return -1;
	buffer[count] = 0;
	return 0;
}

static int check_resolved_mount_target(void)
{
	static const char old_entry[] =
		"tmpfs /tmp/mount-relative/mnt tmpfs rw,relatime 0 0\n";
	static const char new_entry[] =
		"tmpfs /tmp/mount-relative-moved/mnt tmpfs rw,relatime 0 0\n";
	char buffer[4096];

	rmdir(RELATIVE_MOVED "/mnt");
	rmdir(RELATIVE_MOVED);
	rmdir(RELATIVE_BASE "/mnt");
	rmdir(RELATIVE_BASE);
	if (mkdir(RELATIVE_BASE, 0700) || chdir(RELATIVE_BASE) ||
	    mkdir("mnt", 0700) || mount("tmpfs", "mnt", "tmpfs", 0, NULL))
		return -1;
	if (read_mount_snapshot(buffer, sizeof(buffer)) ||
	    !strstr(buffer, old_entry))
		return -1;
	if (chdir("/") || rename(RELATIVE_BASE, RELATIVE_MOVED))
		return -1;
	if (read_mount_snapshot(buffer, sizeof(buffer)) ||
	    !strstr(buffer, new_entry) || strstr(buffer, old_entry))
		return -1;
	if (umount2(RELATIVE_MOVED "/mnt", 0) ||
	    rmdir(RELATIVE_MOVED "/mnt") || rmdir(RELATIVE_MOVED))
		return -1;
	return 0;
}

static int check_cwd_busy(void)
{
	if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL) ||
	    chdir(MOUNT_POINT))
		return -1;
	errno = 0;
	if (expect_error(umount2(MOUNT_POINT, 0), EBUSY))
		return -1;
	if (chdir("/") || umount2(MOUNT_POINT, 0))
		return -1;
	return 0;
}

static int check_mapping_busy(void)
{
	char *mapping;
	int fd;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL))
		return -1;
	fd = open(MOUNT_POINT "/mapped", O_CREAT | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, 4096))
		return -1;
	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return -1;
	mapping[0] = 'M';
	if (msync(mapping, 4096, MS_SYNC))
		return -1;
	errno = 0;
	if (expect_error(umount2(MOUNT_POINT, 0), EBUSY))
		return -1;
	if (munmap(mapping, 4096) || close(fd) ||
	    umount2(MOUNT_POINT, 0))
		return -1;
	return 0;
}

static int check_nested_mounts(void)
{
	if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL) ||
	    mkdir(MOUNT_POINT "/child", 0700) ||
	    mount("tmpfs", MOUNT_POINT "/child", "tmpfs", 0, NULL))
		return -1;
	errno = 0;
	if (expect_error(umount2(MOUNT_POINT, 0), EBUSY))
		return -1;
	if (umount2(MOUNT_POINT "/child", 0) ||
	    umount2(MOUNT_POINT, 0))
		return -1;
	return 0;
}

static int check_mountpoint_mutations(void)
{
	static const char replacement[] = "/tmp/mount-runtime-replacement";
	static const char moved[] = "/tmp/mount-runtime-moved";

	if (mkdir(replacement, 0700) ||
	    mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL))
		return -1;
	errno = 0;
	if (expect_error(rename(replacement, MOUNT_POINT), EBUSY))
		return -1;
	errno = 0;
	if (expect_error(rename(MOUNT_POINT, moved), EBUSY))
		return -1;
	errno = 0;
	if (expect_error(rmdir(MOUNT_POINT), EBUSY))
		return -1;
	if (umount2(MOUNT_POINT, 0) || rmdir(replacement))
		return -1;
	return 0;
}

static int check_repeated_mounts(void)
{
	int fd, iteration;

	for (iteration = 0; iteration < 32; iteration++) {
		if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL))
			return -1;
		fd = open(MOUNT_POINT "/linked", O_CREAT | O_RDWR, 0600);
		if (fd < 0 || close(fd) ||
		    link(MOUNT_POINT "/linked", MOUNT_POINT "/alias") ||
		    umount2(MOUNT_POINT, 0))
			return -1;
	}
	return 0;
}

static void *concurrent_sync_worker(void *argument)
{
	struct sync_worker *worker = argument;

	while (!atomic_load_explicit(worker->start, memory_order_acquire))
		usleep(1000);
	while (!atomic_load_explicit(worker->stop, memory_order_acquire))
		sync();
	return NULL;
}

static int check_concurrent_sync_unmount(void)
{
	atomic_int start, stop;
	struct sync_worker worker = {
		.start = &start,
		.stop = &stop,
	};
	pthread_t thread;
	int failed = 0, iteration;

	atomic_init(&start, 0);
	atomic_init(&stop, 0);
	if (pthread_create(&thread, NULL, concurrent_sync_worker, &worker))
		return -1;
	atomic_store_explicit(&start, 1, memory_order_release);
	for (iteration = 0; iteration < 64; iteration++) {
		if (mount("tmpfs", MOUNT_POINT, "tmpfs", 0, NULL) ||
		    umount2(MOUNT_POINT, 0)) {
			failed = 1;
			break;
		}
	}
	atomic_store_explicit(&stop, 1, memory_order_release);
	pthread_join(thread, NULL);
	return failed ? -1 : 0;
}

static int same_time(const struct timespec *left,
		     const struct timespec *right)
{
	return left->tv_sec == right->tv_sec &&
	       left->tv_nsec == right->tv_nsec;
}

static int set_access_times(const char *path, time_t atime, time_t mtime)
{
	struct timespec times[2] = {
		{ atime, 123456789 },
		{ mtime, 987654321 },
	};

	return utimensat(AT_FDCWD, path, times, 0);
}

static int make_access_file(void)
{
	int fd = open(MOUNT_POINT "/accessed", O_CREAT | O_TRUNC | O_RDWR,
		      0600);

	if (fd < 0 || write(fd, "x", 1) != 1 || close(fd))
		return -1;
	return 0;
}

static int read_access_file(void)
{
	char byte;
	int fd = open(MOUNT_POINT "/accessed", O_RDONLY);
	int result;

	if (fd < 0)
		return -1;
	result = read(fd, &byte, 1) == 1 && !close(fd) ? 0 : -1;
	return result;
}

static int check_file_atime(unsigned long flags, int future,
			    int expect_change)
{
	struct timespec now;
	struct stat before, after;
	time_t atime, mtime;
	int result = -1;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", flags, NULL) ||
	    make_access_file() || clock_gettime(CLOCK_REALTIME, &now))
		return -1;
	atime = future ? now.tv_sec + 3600 : 1;
	mtime = future ? now.tv_sec - 1 : now.tv_sec;
	if (set_access_times(MOUNT_POINT "/accessed", atime, mtime) ||
	    stat(MOUNT_POINT "/accessed", &before) ||
	    read_access_file() || stat(MOUNT_POINT "/accessed", &after))
		goto out;
	if (same_time(&before.st_atim, &after.st_atim) == expect_change)
		goto out;
	result = 0;
out:
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return result;
}

enum eof_read_operation {
	EOF_READ,
	EOF_PREAD,
	EOF_READV,
	EOF_PREADV,
	ZERO_READ,
};

static int check_eof_read(int fd, enum eof_read_operation operation,
			  time_t now)
{
	struct iovec vector;
	struct stat before, after;
	char byte;
	int result;

	if (set_access_times(MOUNT_POINT "/accessed", now + 3600,
			     now - 1) ||
	    stat(MOUNT_POINT "/accessed", &before))
		return -1;
	vector.iov_base = &byte;
	vector.iov_len = 1;
	switch (operation) {
	case EOF_READ:
		result = lseek(fd, 1, SEEK_SET) == 1 &&
			 read(fd, &byte, 1) == 0 ? 0 : -1;
		break;
	case EOF_PREAD:
		result = pread(fd, &byte, 1, 1) == 0 ? 0 : -1;
		break;
	case EOF_READV:
		result = lseek(fd, 1, SEEK_SET) == 1 &&
			 readv(fd, &vector, 1) == 0 ? 0 : -1;
		break;
	case EOF_PREADV:
		result = preadv(fd, &vector, 1, 1) == 0 ? 0 : -1;
		break;
	case ZERO_READ:
		result = read(fd, &byte, 0) == 0 ? 0 : -1;
		break;
	default:
		return -1;
	}
	if (result || stat(MOUNT_POINT "/accessed", &after))
		return -1;
	if (operation == ZERO_READ)
		return same_time(&before.st_atim, &after.st_atim) ? 0 : -1;
	return same_time(&before.st_atim, &after.st_atim) ? -1 : 0;
}

static int check_eof_atime(void)
{
	struct timespec now;
	int fd = -1, operation;
	int result = -1;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_STRICTATIME, NULL) ||
	    make_access_file() || clock_gettime(CLOCK_REALTIME, &now))
		return -1;
	fd = open(MOUNT_POINT "/accessed", O_RDONLY);
	if (fd < 0)
		goto out;
	for (operation = EOF_READ; operation <= ZERO_READ; operation++) {
		if (check_eof_read(fd, operation, now.tv_sec))
			goto out;
	}
	result = 0;
out:
	if (fd >= 0 && close(fd))
		result = -1;
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return result;
}

static int check_nodiratime(void)
{
	struct timespec now;
	struct stat before, after;
	struct dirent *entry;
	DIR *directory;
	int result = -1;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs",
		  MS_STRICTATIME | MS_NODIRATIME, NULL) ||
	    make_access_file() || clock_gettime(CLOCK_REALTIME, &now) ||
	    set_access_times(MOUNT_POINT, now.tv_sec + 3600,
			     now.tv_sec - 1) ||
	    stat(MOUNT_POINT, &before))
		return -1;
	directory = opendir(MOUNT_POINT);
	if (!directory)
		goto out;
	do {
		errno = 0;
		entry = readdir(directory);
	} while (entry);
	if (errno || closedir(directory) || stat(MOUNT_POINT, &after))
		goto out;
	if (!same_time(&before.st_atim, &after.st_atim))
		goto out;
	result = 0;
out:
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return result;
}

static int check_directory_eof_atime(void)
{
	char buffer[256];
	struct timespec now;
	struct stat before, after;
	ssize_t count;
	int fd = -1, result = -1;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_STRICTATIME, NULL) ||
	    make_access_file() || clock_gettime(CLOCK_REALTIME, &now))
		return -1;
	fd = open(MOUNT_POINT, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		goto out;
	do {
		count = syscall(SYS_getdents64, fd, buffer, sizeof(buffer));
	} while (count > 0);
	if (count || set_access_times(MOUNT_POINT, now.tv_sec + 3600,
				      now.tv_sec - 1) ||
	    stat(MOUNT_POINT, &before) ||
	    syscall(SYS_getdents64, fd, buffer, sizeof(buffer)) != 0 ||
	    stat(MOUNT_POINT, &after) ||
	    same_time(&before.st_atim, &after.st_atim))
		goto out;
	result = 0;
out:
	if (fd >= 0 && close(fd))
		result = -1;
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return result;
}

static int check_mmap_atime(void)
{
	struct timespec now;
	struct stat before, after;
	volatile unsigned char *mapping;
	unsigned char value;
	int fd, result = -1;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_STRICTATIME, NULL) ||
	    make_access_file() || clock_gettime(CLOCK_REALTIME, &now) ||
	    set_access_times(MOUNT_POINT "/accessed", now.tv_sec + 3600,
			     now.tv_sec - 1) ||
	    stat(MOUNT_POINT "/accessed", &before))
		return -1;
	fd = open(MOUNT_POINT "/accessed", O_RDONLY);
	if (fd < 0)
		goto out;
	mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		goto out;
	}
	value = mapping[0];
	if (munmap((void *)mapping, 4096) || close(fd) ||
	    stat(MOUNT_POINT "/accessed", &after) || value != 'x')
		goto out;
	if (!same_time(&before.st_atim, &after.st_atim))
		result = 0;
out:
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return result;
}

static int time_before(const struct timespec *left,
		       const struct timespec *right)
{
	return left->tv_sec < right->tv_sec ||
	       (left->tv_sec == right->tv_sec &&
		left->tv_nsec < right->tv_nsec);
}

static void *atime_reader(void *argument)
{
	struct atime_worker *worker = argument;
	char byte;
	int fd, index;

	fd = open(worker->path, O_RDONLY);
	if (fd < 0) {
		worker->result = -1;
		atomic_fetch_add_explicit(worker->ready, 1,
					  memory_order_release);
		atomic_fetch_add_explicit(worker->done, 1,
					  memory_order_release);
		return NULL;
	}
	atomic_fetch_add_explicit(worker->ready, 1, memory_order_release);
	while (!atomic_load_explicit(worker->start, memory_order_acquire))
		;
	for (index = 0; index < ATIME_RACE_READS; index++) {
		if (pread(fd, &byte, 1, 0) != 1 || byte != 'x') {
			worker->result = -1;
			break;
		}
	}
	if (close(fd))
		worker->result = -1;
	atomic_fetch_add_explicit(worker->done, 1, memory_order_release);
	return NULL;
}

static int check_concurrent_atime(void)
{
	struct atime_worker workers[ATIME_RACE_THREADS];
	struct timespec previous = { 0 };
	struct stat state;
	atomic_int ready, start, done;
	pthread_t threads[ATIME_RACE_THREADS];
	int failed = 0, index, thread_count = 0;

	atomic_init(&ready, 0);
	atomic_init(&start, 0);
	atomic_init(&done, 0);
	if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_STRICTATIME, NULL) ||
	    make_access_file() ||
	    link(MOUNT_POINT "/accessed", MOUNT_POINT "/alias"))
		return -1;
	for (index = 0; index < ATIME_RACE_THREADS; index++) {
		workers[index] = (struct atime_worker) {
			.path = index & 1 ? MOUNT_POINT "/alias" :
				MOUNT_POINT "/accessed",
			.ready = &ready,
			.start = &start,
			.done = &done,
		};
		if (pthread_create(&threads[index], NULL, atime_reader,
				   &workers[index])) {
			failed = 1;
			break;
		}
		thread_count++;
	}
	while (atomic_load_explicit(&ready, memory_order_acquire) !=
	       thread_count)
		;
	atomic_store_explicit(&start, 1, memory_order_release);
	while (atomic_load_explicit(&done, memory_order_acquire) !=
	       thread_count) {
		if (stat(MOUNT_POINT "/accessed", &state) ||
		    time_before(&state.st_atim, &previous)) {
			failed = 1;
			break;
		}
		previous = state.st_atim;
	}
	for (index = 0; index < thread_count; index++) {
		pthread_join(threads[index], NULL);
		if (workers[index].result)
			failed = 1;
	}
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return failed ? -1 : 0;
}

static int make_directory_entries(const char *directory, char prefix)
{
	char path[128];
	int fd, index;

	for (index = 0; index < DIRECTORY_REBIND_ENTRIES; index++) {
		if (snprintf(path, sizeof(path), "%s/%c%03d", directory,
			     prefix, index) >= (int)sizeof(path))
			return -1;
		fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
		if (fd < 0 || close(fd))
			return -1;
	}
	return 0;
}

static void *directory_rebind_worker(void *argument)
{
	struct directory_rebind_worker *worker = argument;

	atomic_store_explicit(&worker->ready, 1, memory_order_release);
	while (!atomic_load_explicit(&worker->start, memory_order_acquire))
		;
	while (!atomic_load_explicit(&worker->stop, memory_order_acquire)) {
		if (dup2(worker->first_fd, worker->target_fd) < 0 ||
		    dup2(worker->second_fd, worker->target_fd) < 0) {
			worker->result = -1;
			break;
		}
	}
	return NULL;
}

static int directory_buffer_source(const char *buffer, ssize_t length)
{
	size_t offset = 0;
	int source = 0;

	while (offset < (size_t)length) {
		const struct dirent *entry =
			(const struct dirent *)(buffer + offset);

		if (entry->d_reclen < 24 ||
		    entry->d_reclen > (size_t)length - offset)
			return -1;
		if (entry->d_name[0] == 'a')
			source |= 1;
		else if (entry->d_name[0] == 'b')
			source |= 2;
		offset += entry->d_reclen;
	}
	return source;
}

static int check_directory_rebind_atime(void)
{
	static const char first_path[] = MOUNT_POINT "/rebind-a";
	static const char second_path[] = MOUNT_POINT "/rebind-b";
	struct directory_rebind_worker worker;
	struct stat first_before, first_after;
	struct stat second_before, second_after;
	struct timespec now;
	pthread_t thread;
	ssize_t length;
	int failed = 0, source, trial;

	if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_STRICTATIME, NULL) ||
	    mkdir(first_path, 0700) || mkdir(second_path, 0700) ||
	    make_directory_entries(first_path, 'a') ||
	    make_directory_entries(second_path, 'b') ||
	    clock_gettime(CLOCK_REALTIME, &now))
		return -1;
	for (trial = 0; trial < DIRECTORY_REBIND_TRIALS; trial++) {
		worker = (struct directory_rebind_worker) {
			.first_fd = open(first_path, O_RDONLY | O_DIRECTORY),
			.second_fd = open(second_path, O_RDONLY | O_DIRECTORY),
			.target_fd = -1,
		};
		if (worker.first_fd < 0 || worker.second_fd < 0)
			goto fail_trial;
		worker.target_fd = dup(worker.first_fd);
		if (worker.target_fd < 0 ||
		    set_access_times(first_path, now.tv_sec + 3600,
				     now.tv_sec - 1) ||
		    set_access_times(second_path, now.tv_sec + 3600,
				     now.tv_sec - 1) ||
		    stat(first_path, &first_before) ||
		    stat(second_path, &second_before))
			goto fail_trial;
		atomic_init(&worker.ready, 0);
		atomic_init(&worker.start, 0);
		atomic_init(&worker.stop, 0);
		worker.result = 0;
		if (pthread_create(&thread, NULL, directory_rebind_worker,
				   &worker))
			goto fail_trial;
		while (!atomic_load_explicit(&worker.ready,
					    memory_order_acquire))
			;
		atomic_store_explicit(&worker.start, 1, memory_order_release);
		usleep(1000);
		length = syscall(SYS_getdents64, worker.target_fd,
				 directory_rebind_buffer,
				 sizeof(directory_rebind_buffer));
		atomic_store_explicit(&worker.stop, 1, memory_order_release);
		pthread_join(thread, NULL);
		source = length > 0 ?
			directory_buffer_source(directory_rebind_buffer, length) : -1;
		if (worker.result || length <= 0 ||
		    stat(first_path, &first_after) ||
		    stat(second_path, &second_after) ||
		    (source != 1 && source != 2) ||
		    (source == 1 &&
		     (same_time(&first_before.st_atim, &first_after.st_atim) ||
		      !same_time(&second_before.st_atim,
				 &second_after.st_atim))) ||
		    (source == 2 &&
		     (same_time(&second_before.st_atim,
				&second_after.st_atim) ||
		      !same_time(&first_before.st_atim, &first_after.st_atim))))
			failed = 1;
		if (close(worker.target_fd) || close(worker.second_fd) ||
		    close(worker.first_fd))
			failed = 1;
		if (failed)
			break;
		continue;

fail_trial:
		if (worker.target_fd >= 0)
			close(worker.target_fd);
		if (worker.second_fd >= 0)
			close(worker.second_fd);
		if (worker.first_fd >= 0)
			close(worker.first_fd);
		failed = 1;
		break;
	}
	if (umount2(MOUNT_POINT, 0))
		return -1;
	return failed ? -1 : 0;
}

static int check_atime_policies(void)
{
	errno = 0;
	if (expect_error(mount("tmpfs", MOUNT_POINT, "tmpfs",
			       MS_NOATIME | MS_STRICTATIME, NULL), EINVAL))
		return -1;
	if (check_file_atime(0, 1, 0) ||
	    check_file_atime(0, 0, 1) ||
	    check_file_atime(MS_NOATIME, 0, 0) ||
	    check_file_atime(MS_STRICTATIME, 1, 1) ||
	    check_nodiratime() || check_directory_eof_atime() ||
	    check_concurrent_atime() || check_directory_rebind_atime())
		return -1;
	return 0;
}

static int check_fat_atime_policies(void)
{
	int result = -1;

	if (umount2(FAT_POINT, 0))
		return -1;
	errno = 0;
	if (expect_error(mount(fat_device, FAT_POINT, "fat",
			       MS_STRICTATIME, NULL), EOPNOTSUPP))
		goto out;
	errno = 0;
	if (expect_error(mount(fat_device, FAT_POINT, "fat",
			       MS_RELATIME, NULL), EOPNOTSUPP))
		goto out;
	if (mount(fat_device, FAT_POINT, "fat", MS_NOATIME, NULL))
		goto out;
	result = 0;
out:
	if (result && mount(fat_device, FAT_POINT, "fat", 0, NULL))
		return -1;
	return result;
}

static int check_mount_options(void)
{
	char line[256];
	int fat = 0, root = 0, tmp = 0;
	FILE *mounts = fopen("/proc/mounts", "r");

	if (!mounts)
		return -1;
	while (fgets(line, sizeof(line), mounts)) {
		if (strstr(line, " / ext4 rw,noatime "))
			root = 1;
		if (strstr(line, " /tmp tmpfs rw,relatime "))
			tmp = 1;
		if (strstr(line, " /mnt/fat fat rw,noatime "))
			fat = 1;
	}
	return fclose(mounts) || !root || !tmp || !fat ? -1 : 0;
}

static int check_mount_errors(void)
{
	errno = 0;
	if (expect_error(umount2("/", 0), EBUSY)) {
		failure_detail = "root unmount";
		return -1;
	}
	errno = 0;
	if (expect_error(umount2(MOUNT_POINT, 0), EINVAL)) {
		failure_detail = "non-mount unmount";
		return -1;
	}
	errno = 0;
	if (expect_error(mount("tmpfs", MOUNT_POINT, "tmpfs",
				     MS_RDONLY, NULL), EOPNOTSUPP)) {
		failure_detail = "mount flags";
		return -1;
	}
	errno = 0;
	if (expect_error(mount("tmpfs", MOUNT_POINT, "tmpfs", 0,
				     "mode=0700"), EOPNOTSUPP)) {
		failure_detail = "mount data";
		return -1;
	}
	errno = 0;
	if (expect_error(mount(fat_device, MOUNT_POINT, "missing", 0,
				     NULL), ENODEV)) {
		failure_detail = "unknown filesystem";
		return -1;
	}
	errno = 0;
	if (expect_error(mount("/dev/console", MOUNT_POINT, "fat", 0,
				     NULL), ENODEV)) {
		failure_detail = "non-block source";
		return -1;
	}
	return 0;
}

static int check_fat_remount(void)
{
	static const char payload[] = "fat-remount";
	char buffer[sizeof(payload)];
	int fd;

	fd = open(FAT_POINT "/mount-held", O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return -1;
	errno = 0;
	if (expect_error(umount2(FAT_POINT, 0), EBUSY)) {
		close(fd);
		return -1;
	}
	if (close(fd) || unlink(FAT_POINT "/mount-held") ||
	    umount2(FAT_POINT, 0))
		return -1;
	if (mount(fat_device, FAT_POINT, "fat", 0, NULL))
		return -1;
	fd = open(FAT_POINT "/mount-remounted",
		  O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || write(fd, payload, sizeof(payload)) != sizeof(payload) ||
	    lseek(fd, 0, SEEK_SET) != 0 ||
	    read(fd, buffer, sizeof(buffer)) != sizeof(buffer) ||
	    memcmp(buffer, payload, sizeof(payload)) || fsync(fd) || close(fd) ||
	    unlink(FAT_POINT "/mount-remounted"))
		return -1;
	return 0;
}

static void *concurrent_mount_worker(void *argument)
{
	struct mount_worker *worker = argument;

	while (!atomic_load_explicit(worker->start, memory_order_acquire))
		usleep(1000);
	if (atomic_load_explicit(worker->abort, memory_order_relaxed))
		return NULL;
	errno = 0;
	worker->result = mount(fat_device, worker->target, "fat", 0, NULL);
	worker->error = errno;
	return NULL;
}

static int check_concurrent_fat_mount(void)
{
	struct mount_worker workers[2] = {
		{ .target = FAT_RACE_A },
		{ .target = FAT_RACE_B },
	};
	const char *winner = NULL;
	atomic_int start, abort;
	pthread_t threads[2];
	int created = 0, failed = 0, index;

	if (umount2(FAT_POINT, 0) || mkdir(FAT_RACE_A, 0700) ||
	    mkdir(FAT_RACE_B, 0700))
		return -1;
	atomic_init(&start, 0);
	atomic_init(&abort, 0);
	for (index = 0; index < 2; index++) {
		workers[index].start = &start;
		workers[index].abort = &abort;
		workers[index].result = -1;
		workers[index].error = 0;
		if (pthread_create(&threads[index], NULL,
		                   concurrent_mount_worker, &workers[index])) {
			atomic_store_explicit(&abort, 1, memory_order_relaxed);
			atomic_store_explicit(&start, 1, memory_order_release);
			failed = 1;
			break;
		}
		created++;
	}
	atomic_store_explicit(&start, 1, memory_order_release);
	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
	if (created != 2)
		failed = 1;
	for (index = 0; index < created; index++) {
		if (!workers[index].result) {
			if (winner)
				failed = 1;
			winner = workers[index].target;
		} else if (workers[index].error != EBUSY) {
			failed = 1;
		}
	}
	if (!winner)
		failed = 1;
	if (winner && umount2(winner, 0))
		failed = 1;
	if (mount(fat_device, FAT_POINT, "fat", 0, NULL) ||
	    rmdir(FAT_RACE_A) || rmdir(FAT_RACE_B))
		failed = 1;
	return failed ? -1 : 0;
}

int main(void)
{
	if (mkdir(MOUNT_POINT, 0700) && errno != EEXIST)
		return fail("mountpoint");
	if (find_fat_device())
		return fail("fat device");
	if (check_permission())
		return fail("permission");
	if (check_mount_errors())
		return fail(failure_detail ? failure_detail : "errors");
	if (check_proc_mount())
		return fail("proc mount");
	if (check_resolved_mount_target())
		return fail("resolved mount target");
	if (check_mount_options())
		return fail("mount options");
	if (check_atime_policies())
		return fail("atime policies");
	if (check_eof_atime())
		return fail("EOF atime");
	if (check_mmap_atime())
		return fail("mmap atime");
	if (check_fat_atime_policies())
		return fail("FAT atime policies");
	if (check_open_busy())
		return fail("open busy");
	if (check_cwd_busy())
		return fail("cwd busy");
	if (check_mapping_busy())
		return fail("mapping busy");
	if (check_nested_mounts())
		return fail("nested mounts");
	if (check_mountpoint_mutations())
		return fail("mountpoint mutations");
	if (check_repeated_mounts())
		return fail("repeated mounts");
	if (check_concurrent_sync_unmount())
		return fail("concurrent sync unmount");
	if (check_fat_remount())
		return fail("fat remount");
	if (check_concurrent_fat_mount())
		return fail("concurrent fat mount");
	if (rmdir(MOUNT_POINT))
		return fail("cleanup");
	puts("MOUNT_RUNTIME_OK");
	return 0;
}
