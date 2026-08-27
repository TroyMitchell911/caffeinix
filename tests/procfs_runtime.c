#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 8192
#define LOAD_WORKERS 8
#define LOAD_WORK_NS 6500000000LL
#define CHILD_CPU_WORK_NS 100000000LL
#define SHARED_READ_ROUNDS 16
#define SHARED_READ_WORKERS 8
#define SYSINFO_LOAD_SHIFT 16
#define TRANSPORT_SOCKET_COUNT 12

static void fail(const char *test)
{
	printf("PROCFS_RUNTIME_FAIL %s errno=%d\n", test, errno);
	exit(1);
}

static ssize_t read_file(const char *path, char *buffer, size_t size)
{
	ssize_t count, total = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	while ((size_t)total < size - 1) {
		count = read(fd, buffer + total, size - 1 - total);
		if (count < 0) {
			close(fd);
			return -1;
		}
		if (!count)
			break;
		total += count;
	}
	buffer[total] = 0;
	close(fd);
	return total;
}

struct shared_read_worker {
	pthread_barrier_t *barrier;
	ssize_t count;
	int fd;
	int failed;
};

static void *shared_read_worker(void *argument)
{
	struct shared_read_worker *worker = argument;
	char byte;
	ssize_t count;

	pthread_barrier_wait(worker->barrier);
	while ((count = read(worker->fd, &byte, 1)) > 0)
		worker->count += count;
	if (count < 0)
		worker->failed = 1;
	return 0;
}

static void test_shared_read_position(void)
{
	struct shared_read_worker workers[SHARED_READ_WORKERS];
	pthread_t threads[SHARED_READ_WORKERS];
	pthread_barrier_t barrier;
	char snapshot[BUFFER_SIZE];
	ssize_t expected, total;
	int fd, index, round;

	for (round = 0; round < SHARED_READ_ROUNDS; round++) {
		fd = open("/proc/self/stat", O_RDONLY);
		if (fd < 0 ||
		    (expected = pread(fd, snapshot, sizeof(snapshot), 0)) <= 0 ||
		    pthread_barrier_init(&barrier, 0,
					 SHARED_READ_WORKERS + 1))
			fail("shared read setup");
		memset(workers, 0, sizeof(workers));
		for (index = 0; index < SHARED_READ_WORKERS; index++) {
			workers[index].barrier = &barrier;
			workers[index].fd = dup(fd);
			if (workers[index].fd < 0 ||
			    pthread_create(&threads[index], 0, shared_read_worker,
					   &workers[index]))
				fail("shared read worker");
		}
		pthread_barrier_wait(&barrier);
		total = 0;
		for (index = 0; index < SHARED_READ_WORKERS; index++) {
			if (pthread_join(threads[index], 0) ||
			    workers[index].failed || close(workers[index].fd))
				fail("shared read join");
			total += workers[index].count;
		}
		if (total != expected || close(fd) ||
		    pthread_barrier_destroy(&barrier))
			fail("shared read position");
	}
}

static unsigned long read_meminfo_value(const char *buffer,
					const char *name)
{
	const char *line = strstr(buffer, name);
	unsigned long value;

	if (!line || sscanf(line + strlen(name), ": %lu kB", &value) != 1)
		fail("meminfo parse");
	return value;
}

static unsigned long read_stat_value(const char *buffer, const char *name)
{
	const char *line = strstr(buffer, name);
	unsigned long value;

	if (!line || (line != buffer && line[-1] != '\n') ||
	    sscanf(line + strlen(name), " %lu", &value) != 1)
		fail("stat parse");
	return value;
}

static unsigned long read_status_signal(const char *buffer,
					const char *name)
{
	const char *line = strstr(buffer, name);
	unsigned long value;

	if (!line || (line != buffer && line[-1] != '\n') ||
	    sscanf(line + strlen(name), ":\t%lx", &value) != 1)
		fail("status signal parse");
	return value;
}

static void read_self_child_times(unsigned long *user,
				  unsigned long *system)
{
	char buffer[BUFFER_SIZE];
	char *fields;
	char state;
	int ppid, pgid, sid, tty, tty_pgid;
	unsigned long flags, minflt, child_minflt, majflt, child_majflt;
	unsigned long self_user, self_system;

	if (read_file("/proc/self/stat", buffer, sizeof(buffer)) <= 0)
		fail("child time read");
	fields = strrchr(buffer, ')');
	if (!fields ||
	    sscanf(fields + 1,
		   " %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu "
		   "%lu %lu %lu", &state, &ppid, &pgid, &sid, &tty,
		   &tty_pgid, &flags, &minflt, &child_minflt, &majflt,
		   &child_majflt, &self_user, &self_system, user,
		   system) != 15)
		fail("child time parse");
}

static ino_t directory_entry_inode(const char *path, const char *name)
{
	struct dirent *entry;
	DIR *directory;
	ino_t inode = 0;

	directory = opendir(path);
	if (!directory)
		return 0;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, name)) {
			inode = entry->d_ino;
			break;
		}
	}
	if (closedir(directory))
		return 0;
	return inode;
}

static void require_directory_inode(const char *directory,
				    const char *name, const char *path)
{
	struct stat status;
	ino_t inode = directory_entry_inode(directory, name);

	if (!inode || lstat(path, &status) || inode != status.st_ino)
		fail("directory inode identity");
}

static void test_self_and_cmdline(int argc, char **argv)
{
	struct stat terminal;
	char buffer[BUFFER_SIZE], expected[32];
	char cmdline[BUFFER_SIZE];
	char *fields;
	ssize_t count, length;
	size_t offset = 0;
	char state;
	int index, pgid, ppid, sid, tty;

	count = readlink("/proc/self", buffer, sizeof(buffer) - 1);
	if (count < 0)
		fail("self readlink");
	buffer[count] = 0;
	snprintf(expected, sizeof(expected), "%d", getpid());
	if (strcmp(buffer, expected))
		fail("self target");
	if (read_file("/proc/self/stat", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "(procfs-runtime)"))
		fail("self stat");
	fields = strrchr(buffer, ')');
	if (!fields ||
	    sscanf(fields + 1, " %c %d %d %d %d", &state, &ppid, &pgid,
		   &sid, &tty) != 5 || stat("/dev/ttyS0", &terminal) ||
	    tty != (int)terminal.st_rdev)
		fail("self terminal");
	if (read_file("/proc/self/status", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "Name:\tprocfs-runtime\n") ||
	    !strstr(buffer, "Uid:\t0\t0\t0\t0\n"))
		fail("self status");
	length = read_file("/proc/self/cmdline", cmdline, sizeof(cmdline));
	if (length <= 0)
		fail("self cmdline");
	for (index = 0; index < argc; index++) {
		size_t argument_length = strlen(argv[index]) + 1;

		if (offset + argument_length > (size_t)length ||
		    memcmp(cmdline + offset, argv[index], argument_length))
			fail("cmdline arguments");
		offset += argument_length;
	}
	if (offset != (size_t)length)
		fail("cmdline length");
}

static void test_root_directory(void)
{
	struct dirent *entry;
	struct stat self_stat, parent_stat;
	char directory_path[64], path[96], parent_path[96], pid[32];
	DIR *directory;
	int found_pid = 0, found_self = 0, found_net = 0;

	snprintf(pid, sizeof(pid), "%d", getpid());
	directory = opendir("/proc");
	if (!directory)
		fail("proc opendir");
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, pid))
			found_pid = 1;
		else if (!strcmp(entry->d_name, "self"))
			found_self = 1;
		else if (!strcmp(entry->d_name, "net"))
			found_net = 1;
	}
	closedir(directory);
	if (!found_pid || !found_self || !found_net)
		fail("proc readdir");
	snprintf(directory_path, sizeof(directory_path), "/proc/%s", pid);
	require_directory_inode("/proc", pid, directory_path);
	require_directory_inode("/proc", "self", "/proc/self");
	require_directory_inode("/proc", "mounts", "/proc/mounts");
	require_directory_inode("/proc", "net", "/proc/net");
	snprintf(path, sizeof(path), "%s/stat", directory_path);
	require_directory_inode(directory_path, "stat", path);
	snprintf(path, sizeof(path), "%s/status", directory_path);
	require_directory_inode(directory_path, "status", path);
	snprintf(path, sizeof(path), "%s/cmdline", directory_path);
	require_directory_inode(directory_path, "cmdline", path);
	require_directory_inode("/proc/net", "dev", "/proc/net/dev");
	require_directory_inode("/proc/net", "tcp", "/proc/net/tcp");
	snprintf(parent_path, sizeof(parent_path), "/proc/%d/stat", getppid());
	if (stat("/proc/self/stat", &self_stat) ||
	    stat(parent_path, &parent_stat) ||
	    self_stat.st_ino == parent_stat.st_ino)
		fail("per-process inode identity");
}

static void test_pending_signal_split(void)
{
	struct timespec timeout = { 0, 0 };
	unsigned long thread_bit = 1UL << (SIGUSR1 - 1);
	unsigned long shared_bit = 1UL << (SIGUSR2 - 1);
	unsigned long thread_pending, shared_pending;
	char buffer[BUFFER_SIZE];
	sigset_t blocked, previous;
	int first, second;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &blocked, &previous) ||
	    raise(SIGUSR1) || kill(getpid(), SIGUSR2))
		fail("pending signal setup");
	if (read_file("/proc/self/status", buffer, sizeof(buffer)) <= 0)
		fail("pending signal status");
	thread_pending = read_status_signal(buffer, "SigPnd");
	shared_pending = read_status_signal(buffer, "ShdPnd");
	if ((thread_pending & (thread_bit | shared_bit)) != thread_bit ||
	    (shared_pending & (thread_bit | shared_bit)) != shared_bit)
		fail("pending signal split");
	first = sigtimedwait(&blocked, 0, &timeout);
	second = sigtimedwait(&blocked, 0, &timeout);
	if (!((first == SIGUSR1 && second == SIGUSR2) ||
	      (first == SIGUSR2 && second == SIGUSR1)) ||
	    sigprocmask(SIG_SETMASK, &previous, 0))
		fail("pending signal cleanup");
}

static void test_system_files(void)
{
	struct sysinfo information;
	char buffer[BUFFER_SIZE];
	unsigned long total, free_memory;
	unsigned long context_switches, forks, interrupts;
	double uptime, idle, load1, load5, load15;
	unsigned running, processes;
	long cpus;
	int last_pid;

	if (read_file("/proc/mounts", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, " / ext4 rw 0 0\n") ||
	    !strstr(buffer, "devfs /dev devfs rw 0 0\n") ||
	    !strstr(buffer, "tmpfs /tmp tmpfs rw 0 0\n") ||
	    !strstr(buffer, "proc /proc proc rw 0 0\n"))
		fail("mount snapshot");
	if (sysinfo(&information))
		fail("sysinfo");
	if (read_file("/proc/meminfo", buffer, sizeof(buffer)) <= 0)
		fail("meminfo read");
	total = read_meminfo_value(buffer, "MemTotal");
	free_memory = read_meminfo_value(buffer, "MemFree");
	if (total * 1024 != information.totalram ||
	    free_memory * 1024 > information.freeram + 1024 * 1024 ||
	    information.freeram > free_memory * 1024 + 1024 * 1024)
		fail("meminfo accounting");
	if (read_file("/proc/uptime", buffer, sizeof(buffer)) <= 0 ||
	    sscanf(buffer, "%lf %lf", &uptime, &idle) != 2 ||
	    uptime < information.uptime - 1 ||
	    uptime > information.uptime + 1)
		fail("uptime accounting");
	cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpus < 1 || idle < 0 || idle > (uptime + 1) * cpus)
		fail("idle accounting");
	if (read_file("/proc/stat", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "cpu  ") || !strstr(buffer, "btime ") ||
	    !strstr(buffer, "procs_running "))
		fail("stat snapshot");
	interrupts = read_stat_value(buffer, "intr");
	context_switches = read_stat_value(buffer, "ctxt");
	forks = read_stat_value(buffer, "processes");
	if (!interrupts || !context_switches || !forks)
		fail("stat accounting");
	if (read_file("/proc/loadavg", buffer, sizeof(buffer)) <= 0 ||
	    sscanf(buffer, "%lf %lf %lf %u/%u %d", &load1, &load5,
		   &load15, &running, &processes, &last_pid) != 6 ||
	    load1 < 0 || load5 < 0 || load15 < 0 ||
	    !running || !processes || last_pid < getpid())
		fail("loadavg snapshot");
	if (read_file("/proc/net/dev", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "Inter-") || !strstr(buffer, "lo:"))
		fail("net device snapshot");
	if (read_file("/proc/net/tcp", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "local_address") ||
	    read_file("/proc/net/udp", buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, "local_address"))
		fail("transport snapshot");
}

static void test_mount_escaping(void)
{
	static const char target[] =
		"/tmp/proc mount\tline\nslash\\path";
	static const char expected[] =
		"tmpfs /tmp/proc\\040mount\\011line\\012slash\\134path "
		"tmpfs rw 0 0\n";
	char buffer[BUFFER_SIZE];
	int found;

	if (mkdir(target, 0700))
		fail("escaped mount mkdir");
	if (mount("tmpfs", target, "tmpfs", 0, 0)) {
		rmdir(target);
		fail("escaped mount");
	}
	found = read_file("/proc/mounts", buffer, sizeof(buffer)) > 0 &&
		strstr(buffer, expected);
	if (umount2(target, 0) || rmdir(target))
		fail("escaped mount cleanup");
	if (!found)
		fail("escaped mount snapshot");
}

static double read_load_one(void)
{
	char buffer[BUFFER_SIZE];
	double load;

	if (read_file("/proc/loadavg", buffer, sizeof(buffer)) <= 0 ||
	    sscanf(buffer, "%lf", &load) != 1)
		fail("loadavg read");
	return load;
}

struct load_blocker {
	int write_fd;
	pid_t readers[2];
};

static unsigned long read_blocked_tasks(void)
{
	char buffer[BUFFER_SIZE];

	if (read_file("/proc/stat", buffer, sizeof(buffer)) <= 0)
		fail("blocked task read");
	return read_stat_value(buffer, "procs_blocked");
}

static void load_blocked_reader(int read_fd, int write_fd)
{
	char byte;
	struct iovec vector = {
		.iov_base = &byte,
		.iov_len = 1,
	};

	close(write_fd);
	_exit(readv(read_fd, &vector, 1) == 1 ? 0 : 1);
}

static int release_load_blockers(struct load_blocker *blockers, int count)
{
	int index, reader, status, result = 0;

	for (index = 0; index < count; index++) {
		if (blockers[index].write_fd < 0)
			continue;
		if (write(blockers[index].write_fd, "xx", 2) != 2)
			result = -1;
		if (close(blockers[index].write_fd))
			result = -1;
		blockers[index].write_fd = -1;
	}
	for (index = 0; index < count; index++) {
		for (reader = 0; reader < 2; reader++) {
			if (blockers[index].readers[reader] <= 0)
				continue;
			if (waitpid(blockers[index].readers[reader], &status, 0) !=
			    blockers[index].readers[reader] || !WIFEXITED(status) ||
			    WEXITSTATUS(status))
				result = -1;
		}
	}
	return result;
}

static void test_blocked_loadavg(void)
{
	struct load_blocker blockers[LOAD_WORKERS];
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
	double before = read_load_one(), after;
	int attempt, index, reader;

	memset(blockers, 0, sizeof(blockers));
	for (index = 0; index < LOAD_WORKERS; index++)
		blockers[index].write_fd = -1;
	for (index = 0; index < LOAD_WORKERS; index++) {
		int descriptors[2];

		if (pipe(descriptors))
			break;
		blockers[index].write_fd = descriptors[1];
		for (reader = 0; reader < 2; reader++) {
			blockers[index].readers[reader] = fork();
			if (blockers[index].readers[reader] < 0)
				break;
			if (!blockers[index].readers[reader])
				load_blocked_reader(descriptors[0], descriptors[1]);
		}
		close(descriptors[0]);
		if (reader != 2)
			break;
	}
	if (index != LOAD_WORKERS) {
		release_load_blockers(blockers, LOAD_WORKERS);
		fail("blocked loadavg fork");
	}
	for (attempt = 0; attempt < 200; attempt++) {
		if (read_blocked_tasks() >= LOAD_WORKERS)
			break;
		if (nanosleep(&delay, 0))
			break;
	}
	if (attempt == 200) {
		release_load_blockers(blockers, LOAD_WORKERS);
		fail("blocked loadavg state");
	}
	delay.tv_sec = 6;
	delay.tv_nsec = 500000000;
	if (nanosleep(&delay, 0)) {
		release_load_blockers(blockers, LOAD_WORKERS);
		fail("blocked loadavg sleep");
	}
	after = read_load_one();
	if (release_load_blockers(blockers, LOAD_WORKERS))
		fail("blocked loadavg cleanup");
	if (after < before + 0.25)
		fail("blocked loadavg sampling");
}

static void test_loadavg_sampling(void)
{
	struct sysinfo before_info, after_info;
	double before, after, syscall_load, difference;
	pid_t children[LOAD_WORKERS];
	int index, status;

	if (sysinfo(&before_info))
		fail("loadavg sysinfo before");
	before = read_load_one();
	for (index = 0; index < LOAD_WORKERS; index++) {
		children[index] = fork();
		if (children[index] < 0)
			fail("loadavg fork");
		if (!children[index]) {
			struct timespec start, now;

			if (clock_gettime(CLOCK_MONOTONIC, &start))
				_exit(1);
			do {
				if (clock_gettime(CLOCK_MONOTONIC, &now))
					_exit(1);
			} while ((now.tv_sec - start.tv_sec) * 1000000000LL +
				 now.tv_nsec - start.tv_nsec < LOAD_WORK_NS);
			_exit(0);
		}
	}
	for (index = 0; index < LOAD_WORKERS; index++) {
		if (waitpid(children[index], &status, 0) != children[index] ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			fail("loadavg wait");
	}
	after = read_load_one();
	if (sysinfo(&after_info))
		fail("loadavg sysinfo after");
	if (after < before + 0.25 ||
	    after_info.loads[0] <= before_info.loads[0])
		fail("periodic loadavg sampling");
	syscall_load = (double)after_info.loads[0] /
		       (1UL << SYSINFO_LOAD_SHIFT);
	difference = syscall_load - read_load_one();
	if (difference < 0)
		difference = -difference;
	if (difference > 0.02)
		fail("sysinfo loadavg consistency");
}

static unsigned long read_fork_count(void)
{
	char buffer[BUFFER_SIZE];

	if (read_file("/proc/stat", buffer, sizeof(buffer)) <= 0)
		fail("fork count read");
	return read_stat_value(buffer, "processes");
}

static void test_fork_counter(void)
{
	unsigned long before = read_fork_count();
	pid_t child = fork();

	if (child < 0)
		fail("counter fork");
	if (!child)
		_exit(0);
	if (waitpid(child, 0, 0) != child)
		fail("counter wait");
	if (read_fork_count() != before + 1)
		fail("fork accounting");
}

static void test_child_cpu_accounting(void)
{
	unsigned long before_user, before_system;
	unsigned long after_user, after_system;
	pid_t child;
	int status;

	read_self_child_times(&before_user, &before_system);
	child = fork();
	if (child < 0)
		fail("child time fork");
	if (!child) {
		struct timespec start, now;

		if (clock_gettime(CLOCK_MONOTONIC, &start))
			_exit(1);
		do {
			if (clock_gettime(CLOCK_MONOTONIC, &now))
				_exit(1);
		} while ((now.tv_sec - start.tv_sec) * 1000000000LL +
			 now.tv_nsec - start.tv_nsec < CHILD_CPU_WORK_NS);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		fail("child time wait");
	read_self_child_times(&after_user, &after_system);
	if (after_user + after_system <= before_user + before_system)
		fail("child time accounting");
}

static void test_zombie_process_count(void)
{
	struct timespec delay = { 0, 1000000 };
	struct sysinfo before, zombie, reaped;
	char buffer[BUFFER_SIZE], path[64];
	int attempt, status;
	pid_t child;

	if (sysinfo(&before))
		fail("zombie count before");
	child = fork();
	if (child < 0)
		fail("zombie count fork");
	if (!child)
		_exit(0);
	snprintf(path, sizeof(path), "/proc/%d/status", child);
	for (attempt = 0; attempt < 1000; attempt++) {
		if (read_file(path, buffer, sizeof(buffer)) > 0 &&
		    strstr(buffer, "State:\tZ (zombie)\n"))
			break;
		nanosleep(&delay, 0);
	}
	if (attempt == 1000 || sysinfo(&zombie) ||
	    zombie.procs != before.procs + 1)
		fail("zombie process count");
	snprintf(path, sizeof(path), "/proc/%d/cmdline", child);
	if (read_file(path, buffer, sizeof(buffer)) != 0)
		fail("zombie cmdline");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) || sysinfo(&reaped) ||
	    reaped.procs != before.procs)
		fail("reaped process count");
}

static void test_open_lifetime(void)
{
	char before[BUFFER_SIZE], after[BUFFER_SIZE], path[64], byte;
	ssize_t first, second;
	int ready[2], fd, status;
	pid_t child;

	if (pipe(ready))
		fail("lifetime pipe");
	child = fork();
	if (child < 0)
		fail("lifetime fork");
	if (!child) {
		close(ready[0]);
		byte = 'x';
		if (write(ready[1], &byte, 1) != 1)
			_exit(2);
		for (;;)
			pause();
	}
	close(ready[1]);
	if (read(ready[0], &byte, 1) != 1)
		fail("lifetime ready");
	close(ready[0]);
	snprintf(path, sizeof(path), "/proc/%d/stat", child);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail("lifetime open");
	first = read(fd, before, sizeof(before));
	if (first <= 0 || lseek(fd, 0, SEEK_SET) != 0)
		fail("lifetime first read");
	if (kill(child, SIGKILL) || waitpid(child, &status, 0) != child ||
	    !WIFSIGNALED(status))
		fail("lifetime reap");
	second = read(fd, after, sizeof(after));
	close(fd);
	if (second != first || memcmp(before, after, first))
		fail("stable open snapshot");
	errno = 0;
	if (open(path, O_RDONLY) >= 0 || errno != ENOENT)
		fail("reaped process lookup");
}

static void test_transport_entry(int type, const char *path)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t length = sizeof(address);
	char buffer[BUFFER_SIZE], endpoint[32];
	int fd;

	fd = socket(AF_INET, type, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&address,
			    sizeof(address)) < 0)
		fail(type == SOCK_STREAM ? "tcp bind" : "udp bind");
	if (type == SOCK_STREAM && listen(fd, 1) < 0)
		fail("tcp listen");
	if (getsockname(fd, (struct sockaddr *)&address, &length) < 0)
		fail(type == SOCK_STREAM ? "tcp name" : "udp name");
	snprintf(endpoint, sizeof(endpoint), "%08x:%04x",
		 address.sin_addr.s_addr, ntohs(address.sin_port));
	if (read_file(path, buffer, sizeof(buffer)) <= 0 ||
	    !strstr(buffer, endpoint))
		fail(type == SOCK_STREAM ? "tcp snapshot" : "udp snapshot");
	close(fd);
	if (read_file(path, buffer, sizeof(buffer)) <= 0 ||
	    strstr(buffer, endpoint))
		fail(type == SOCK_STREAM ? "tcp release" : "udp release");
}

static void socket_endpoint(int fd, int peer, char *endpoint, size_t size)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	int result = peer ?
		getpeername(fd, (struct sockaddr *)&address, &length) :
		getsockname(fd, (struct sockaddr *)&address, &length);

	if (result < 0)
		fail("tcp endpoint");
	snprintf(endpoint, size, "%08x:%04x", address.sin_addr.s_addr,
		 ntohs(address.sin_port));
}

static int tcp_state(const char *local, const char *remote)
{
	char buffer[BUFFER_SIZE], endpoints[80];
	char *entry;
	unsigned int state;

	if (read_file("/proc/net/tcp", buffer, sizeof(buffer)) <= 0)
		return -1;
	snprintf(endpoints, sizeof(endpoints), "%s %s", local, remote);
	entry = strstr(buffer, endpoints);
	if (!entry || sscanf(entry + strlen(endpoints), " %x", &state) != 1)
		return -1;
	return state;
}

static void test_tcp_states(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t length = sizeof(address);
	char client_local[32], client_remote[32];
	char server_local[32], server_remote[32];
	char byte;
	int accepted, client, listener, state;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address,
				 sizeof(address)) || listen(listener, 1) ||
	    getsockname(listener, (struct sockaddr *)&address, &length))
		fail("tcp state listener");
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0 || connect(client, (struct sockaddr *)&address,
				  sizeof(address)))
		fail("tcp state connect");
	accepted = accept(listener, NULL, NULL);
	if (accepted < 0)
		fail("tcp state accept");
	socket_endpoint(client, 0, client_local, sizeof(client_local));
	socket_endpoint(client, 1, client_remote, sizeof(client_remote));
	socket_endpoint(accepted, 0, server_local, sizeof(server_local));
	socket_endpoint(accepted, 1, server_remote, sizeof(server_remote));
	if (tcp_state(client_local, client_remote) != 0x01 ||
	    tcp_state(server_local, server_remote) != 0x01)
		fail("tcp established state");
	if (shutdown(accepted, SHUT_WR) || read(client, &byte, 1) != 0)
		fail("tcp close setup");
	state = tcp_state(server_local, server_remote);
	if (tcp_state(client_local, client_remote) != 0x08 ||
	    (state != 0x04 && state != 0x05))
		fail("tcp closing state");
	close(accepted);
	close(client);
	close(listener);
}

static void test_transport_population(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t length;
	char buffer[BUFFER_SIZE];
	char endpoints[TRANSPORT_SOCKET_COUNT][32];
	struct stat stats[TRANSPORT_SOCKET_COUNT];
	int sockets[TRANSPORT_SOCKET_COUNT];
	int index;

	for (index = 0; index < TRANSPORT_SOCKET_COUNT; index++) {
		sockets[index] = socket(AF_INET, SOCK_DGRAM, 0);
		address.sin_port = 0;
		length = sizeof(address);
		if (sockets[index] < 0 ||
		    bind(sockets[index], (struct sockaddr *)&address,
			 sizeof(address)) < 0 ||
		    getsockname(sockets[index], (struct sockaddr *)&address,
				&length) < 0 ||
		    fstat(sockets[index], &stats[index]) ||
		    !S_ISSOCK(stats[index].st_mode) || !stats[index].st_ino ||
		    stats[index].st_uid != geteuid())
			fail("transport population setup");
		for (int previous = 0; previous < index; previous++) {
			if (stats[previous].st_ino == stats[index].st_ino)
				fail("transport inode identity");
		}
		snprintf(endpoints[index], sizeof(endpoints[index]),
			 "%08x:%04x", address.sin_addr.s_addr,
			 ntohs(address.sin_port));
	}
	if (read_file("/proc/net/udp", buffer, sizeof(buffer)) <= 0)
		fail("transport population read");
	for (index = 0; index < TRANSPORT_SOCKET_COUNT; index++) {
		char *entry = strstr(buffer, endpoints[index]);
		unsigned int uid;
		unsigned long inode;

		if (!entry ||
		    sscanf(entry, "%*s %*s %*x %*s %*s %*s %u %*u %lu",
			   &uid, &inode) != 2 || uid != stats[index].st_uid ||
		    inode != stats[index].st_ino)
			fail("transport population entry");
		close(sockets[index]);
	}
}

static void test_exit_scan(void)
{
	int round;

	for (round = 0; round < 16; round++) {
		pid_t child = fork();
		DIR *directory;

		if (child < 0)
			fail("scan fork");
		if (!child)
			_exit(0);
		directory = opendir("/proc");
		if (!directory)
			fail("scan opendir");
		while (readdir(directory))
			;
		closedir(directory);
		if (waitpid(child, 0, 0) != child)
			fail("scan wait");
	}
}

int main(int argc, char **argv)
{
	if (argc != 3 || strcmp(argv[1], "alpha") ||
	    strcmp(argv[2], "beta"))
		fail("arguments");
	test_self_and_cmdline(argc, argv);
	test_shared_read_position();
	test_root_directory();
	test_pending_signal_split();
	test_system_files();
	test_mount_escaping();
	test_blocked_loadavg();
	test_loadavg_sampling();
	test_transport_entry(SOCK_STREAM, "/proc/net/tcp");
	test_transport_entry(SOCK_DGRAM, "/proc/net/udp");
	test_tcp_states();
	test_transport_population();
	test_fork_counter();
	test_child_cpu_accounting();
	test_zombie_process_count();
	test_open_lifetime();
	test_exit_scan();
	puts("PROCFS_RUNTIME_OK");
	return 0;
}
