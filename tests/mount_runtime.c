#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOUNT_POINT "/tmp/mount-runtime"
#define PROC_POINT  "/tmp/mount-proc"
#define FAT_POINT   "/mnt/fat"
#define FAT_RACE_A  "/tmp/mount-fat-a"
#define FAT_RACE_B  "/tmp/mount-fat-b"
#define RELATIVE_BASE "/tmp/mount-relative"
#define RELATIVE_MOVED "/tmp/mount-relative-moved"

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
		"tmpfs /tmp/mount-relative/mnt tmpfs rw 0 0\n";
	static const char new_entry[] =
		"tmpfs /tmp/mount-relative-moved/mnt tmpfs rw 0 0\n";
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
