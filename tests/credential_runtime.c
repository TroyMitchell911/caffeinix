#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT_PRIVATE "/credential-root-private"
#define ROOT_GROUP   "/credential-root-group"
#define PRIVATE_DIR  "/credential-private-dir"
#define PRIVATE_CHILD "/credential-private-dir/readable"
#define SETGID_DIR    "/tmp/credential-setgid-dir"
#define SETGID_FILE   "/tmp/credential-setgid-dir/file"
#define OWNED_FILE   "/tmp/credential-owned"
#define STICKY_FILE  "/tmp/credential-root-sticky"
#define UMASK_FILE   "/tmp/credential-umask"
#define ROOT_READONLY "/credential-root-readonly"
#define ROOT_WRITABLE "/credential-root-writable"
#define CREDENTIAL_PROGRAM "/bin/setid-exec-runtime"
#define ZERO_MODE_FILE "/tmp/credential-zero-mode"

static int fail(const char *step)
{
	printf("CREDENTIAL_RUNTIME_FAIL %s errno=%d\n", step, errno);
	return 1;
}

static int create_file(const char *path, mode_t mode)
{
	int fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);

	if (fd < 0)
		return -1;
	return close(fd);
}

static int create_data_file(const char *path, mode_t mode)
{
	int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, mode);

	if (fd < 0 || write(fd, "data", 4) != 4 || close(fd))
		return -1;
	return 0;
}

static int status_contains(const char *expected)
{
	char buffer[2048];
	ssize_t count, total = 0;
	int fd = open("/proc/self/status", O_RDONLY);

	if (fd < 0)
		return -1;
	while (total < (ssize_t)sizeof(buffer) - 1) {
		count = read(fd, buffer + total, sizeof(buffer) - 1 - total);
		if (count < 0) {
			close(fd);
			return -1;
		}
		if (!count)
			break;
		total += count;
	}
	buffer[total] = 0;
	return close(fd) || !strstr(buffer, expected) ? -1 : 0;
}

static int check_ids(uid_t uid, gid_t gid)
{
	uid_t real_uid, effective_uid, saved_uid;
	gid_t real_gid, effective_gid, saved_gid;

	if (getresuid(&real_uid, &effective_uid, &saved_uid) ||
	    getresgid(&real_gid, &effective_gid, &saved_gid))
		return -1;
	return real_uid == uid && effective_uid == uid && saved_uid == uid &&
	       real_gid == gid && effective_gid == gid && saved_gid == gid ?
		0 : -1;
}

static int check_filesystem_id_transitions(void)
{
	uid_t fsuid;
	gid_t fsgid;

	if (setresgid(1001, 0, 0) || setresuid(1001, 0, 0) ||
	    (uid_t)syscall(SYS_setfsuid, 2002) != 0 ||
	    (gid_t)syscall(SYS_setfsgid, 2003) != 0)
		return -1;
	if (setresuid(-1, -1, -1) || setresgid(-1, -1, -1))
		return -1;
	fsuid = syscall(SYS_setfsuid, (uid_t)-1);
	fsgid = syscall(SYS_setfsgid, (gid_t)-1);
	if (fsuid != 2002 || fsgid != 2003)
		return -1;
	if (setreuid(-1, -1) || setregid(-1, -1))
		return -1;
	fsuid = syscall(SYS_setfsuid, (uid_t)-1);
	fsgid = syscall(SYS_setfsgid, (gid_t)-1);
	if (fsuid != 2002 || fsgid != 2003)
		return -1;
	if (setresuid(-1, 0, -1) || setresgid(-1, 0, -1))
		return -1;
	fsuid = syscall(SYS_setfsuid, (uid_t)-1);
	fsgid = syscall(SYS_setfsgid, (gid_t)-1);
	if (fsuid || fsgid ||
	    (uid_t)syscall(SYS_setfsuid, 2002) != 0 ||
	    (gid_t)syscall(SYS_setfsgid, 2003) != 0 ||
	    setreuid(-1, 0) || setregid(-1, 0))
		return -1;
	fsuid = syscall(SYS_setfsuid, (uid_t)-1);
	fsgid = syscall(SYS_setfsgid, (gid_t)-1);
	if (fsuid || fsgid || setresgid(0, 0, 0) ||
	    setresuid(0, 0, 0))
		return -1;
	return 0;
}

static int exec_child(void)
{
	if (check_ids(1001, 1001) || getuid() != 1001 ||
	    geteuid() != 1001 || getgid() != 1001 || getegid() != 1001)
		return fail("exec ids");
	if (getauxval(AT_UID) != 1001 || getauxval(AT_EUID) != 1001 ||
	    getauxval(AT_GID) != 1001 || getauxval(AT_EGID) != 1001 ||
	    getauxval(AT_SECURE) != 0)
		return fail("exec auxv");
	return 0;
}

static int setid_exec_child(void)
{
	uid_t real_uid, effective_uid, saved_uid;
	gid_t real_gid, effective_gid, saved_gid;

	if (getresuid(&real_uid, &effective_uid, &saved_uid) ||
	    getresgid(&real_gid, &effective_gid, &saved_gid) ||
	    real_uid != 1001 || effective_uid || saved_uid ||
	    real_gid != 1001 || effective_gid != 4321 ||
	    saved_gid != 4321 ||
	    getauxval(AT_UID) != 1001 || getauxval(AT_EUID) != 0 ||
	    getauxval(AT_GID) != 1001 || getauxval(AT_EGID) != 4321 ||
	    getauxval(AT_SECURE) != 1)
		return fail("set-ID exec credentials");
	return 0;
}

static int setid_exec_hold_child(const char *ready_text,
				 const char *release_text)
{
	char byte = 'R';
	int ready = atoi(ready_text);
	int release = atoi(release_text);

	if (setid_exec_child())
		return 1;
	if (ready < 0 || release < 0 || write(ready, &byte, 1) != 1 ||
	    read(release, &byte, 1) != 1)
		return fail("set-ID exec hold");
	return 0;
}

static int permission_child(pid_t parent)
{
	struct stat stat_buffer;
	struct timespec explicit[2] = {
		{ .tv_sec = 123, .tv_nsec = 456 },
		{ .tv_sec = 789, .tv_nsec = 123 },
	};
	gid_t groups[2] = { 0, 77 };
	int fd;

	if (setresuid(1001, 0, 0))
		return fail("mixed uid");
	if ((uid_t)syscall(SYS_setfsuid, 2002) != 0 ||
	    (gid_t)syscall(SYS_setfsgid, 2003) != 0 ||
	    status_contains("Uid:\t1001\t0\t0\t2002\n") ||
	    status_contains("Gid:\t0\t0\t0\t2003\n") ||
	    (uid_t)syscall(SYS_setfsuid, 0) != 2002 ||
	    (gid_t)syscall(SYS_setfsgid, 0) != 2003)
		return fail("proc mixed ids");
	errno = 0;
	if (access(ROOT_PRIVATE, R_OK) != -1 || errno != EACCES)
		return fail("access real uid");
	errno = 0;
	if (access(PRIVATE_CHILD, R_OK) != -1 || errno != EACCES)
		return fail("access real uid traversal");
	fd = open(ROOT_PRIVATE, O_RDONLY);
	if (fd < 0 || close(fd))
		return fail("open effective uid");
	if (setgroups(2, groups) || setresgid(1001, 1001, 1001) ||
	    setresuid(1001, 1001, 1001) || check_ids(1001, 1001))
		return fail("drop credentials");
	if (getgroups(0, NULL) != 2)
		return fail("group count");
	groups[0] = groups[1] = 0;
	if (getgroups(2, groups) != 2 || groups[0] != 0 || groups[1] != 77)
		return fail("group values");
	if (status_contains("Uid:\t1001\t1001\t1001\t1001\n") ||
	    status_contains("Gid:\t1001\t1001\t1001\t1001\n") ||
	    status_contains("Groups:\t0 77 \n"))
		return fail("proc credentials");
	errno = 0;
	fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (fd >= 0) {
		close(fd);
		return fail("raw socket accepted");
	}
	if (errno != EPERM)
		return fail("raw socket permission");
	errno = 0;
	if (open(ROOT_PRIVATE, O_RDONLY) != -1 || errno != EACCES)
		return fail("private read");
	fd = open(ROOT_GROUP, O_RDONLY);
	if (fd < 0 || close(fd))
		return fail("supplementary group read");
	errno = 0;
	if (chdir(PRIVATE_DIR) != -1 || errno != EACCES)
		return fail("directory search");
	errno = 0;
	if (unlink(STICKY_FILE) != -1 || errno != EPERM)
		return fail("sticky unlink");
	if (create_file(OWNED_FILE, 0666) || stat(OWNED_FILE, &stat_buffer) ||
	    stat_buffer.st_uid != 1001 || stat_buffer.st_gid != 1001 ||
	    (stat_buffer.st_mode & 07777) != 0640)
		return fail("owned create");
	fd = open(SETGID_FILE, O_CREAT | O_EXCL | O_WRONLY, 02777);
	if (fd < 0 || close(fd) || stat(SETGID_FILE, &stat_buffer) ||
	    stat_buffer.st_gid != 4321 || (stat_buffer.st_mode & S_ISGID))
		return fail("setgid create restriction");
	fd = open(ZERO_MODE_FILE, O_CREAT | O_EXCL | O_WRONLY, 0000);
	if (fd < 0 || write(fd, "x", 1) != 1 || close(fd))
		return fail("zero mode create");
	errno = 0;
	if (open(ROOT_READONLY, O_RDONLY | O_TRUNC) != -1 ||
	    errno != EACCES)
		return fail("readonly truncate");
	errno = 0;
	if (utimensat(AT_FDCWD, ROOT_WRITABLE, explicit, 0) != -1 ||
	    errno != EPERM)
		return fail("explicit timestamp permission");
	if (utimensat(AT_FDCWD, ROOT_WRITABLE, NULL, 0))
		return fail("current timestamp permission");
	errno = 0;
	if (setpriority(PRIO_PROCESS, 0, -1) != -1 || errno != EACCES)
		return fail("raise own priority");
	errno = 0;
	if (setpriority(PRIO_PROCESS, parent, 10) != -1 || errno != EPERM)
		return fail("foreign priority");
	errno = 0;
	if (kill(parent, 0) != -1 || errno != EPERM)
		return fail("signal permission");
	errno = 0;
	if (setgroups(0, NULL) != -1 || errno != EPERM)
		return fail("setgroups permission");
	errno = 0;
	if (setuid(0) != -1 || errno != EPERM)
		return fail("setuid permission");
	if ((uid_t)syscall(SYS_setfsuid, 0) != 1001 ||
	    (uid_t)syscall(SYS_setfsuid, (uid_t)-1) != 1001)
		return fail("setfsuid permission");
	return 0;
}

static int wait_success(pid_t child, const char *step)
{
	int status;

	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return fail(step);
	return 0;
}

static int drop_to_unprivileged_user(void)
{
	gid_t group = 1001;

	return setgroups(1, &group) || setresgid(1001, 1001, 1001) ||
	       setresuid(1001, 1001, 1001) ? -1 : 0;
}

static int check_executable_write_exclusion(void)
{
	struct stat stat_buffer;
	char ready_text[16], release_text[16], byte;
	int ready[2], release[2], writer, failed = 0;
	pid_t child;

	writer = open(CREDENTIAL_PROGRAM, O_WRONLY);
	if (writer < 0)
		return -1;
	child = fork();
	if (child < 0) {
		close(writer);
		return -1;
	}
	if (!child) {
		if (drop_to_unprivileged_user())
			_exit(1);
		execl(CREDENTIAL_PROGRAM, "credential-runtime",
		      "setid-exec", NULL);
		_exit(errno == ETXTBSY ? 0 : 1);
	}
	failed = wait_success(child, "write-open exec exclusion");
	if (close(writer))
		failed = 1;
	if (failed)
		return -1;

	if (pipe(ready))
		return -1;
	if (pipe(release)) {
		close(ready[0]);
		close(ready[1]);
		return -1;
	}
	child = fork();
	if (child < 0) {
		close(ready[0]);
		close(ready[1]);
		close(release[0]);
		close(release[1]);
		return -1;
	}
	if (!child) {
		close(ready[0]);
		close(release[1]);
		snprintf(ready_text, sizeof(ready_text), "%d", ready[1]);
		snprintf(release_text, sizeof(release_text), "%d", release[0]);
		if (drop_to_unprivileged_user())
			_exit(1);
		execl(CREDENTIAL_PROGRAM, "credential-runtime",
		      "setid-exec-hold", ready_text, release_text, NULL);
		_exit(1);
	}
	close(ready[1]);
	close(release[0]);
	if (read(ready[0], &byte, 1) != 1)
		failed = 1;
	close(ready[0]);
	if (!failed) {
		errno = 0;
		writer = open(CREDENTIAL_PROGRAM, O_WRONLY);
		if (writer >= 0 || errno != ETXTBSY)
			failed = 1;
		if (writer >= 0)
			close(writer);
		if (stat(CREDENTIAL_PROGRAM, &stat_buffer)) {
			failed = 1;
		} else {
			errno = 0;
			if (truncate(CREDENTIAL_PROGRAM, stat_buffer.st_size) != -1 ||
			    errno != ETXTBSY)
				failed = 1;
		}
	}
	byte = 'X';
	if (write(release[1], &byte, 1) != 1)
		failed = 1;
	close(release[1]);
	if (wait_success(child, "active exec exclusion"))
		failed = 1;
	writer = open(CREDENTIAL_PROGRAM, O_WRONLY);
	if (writer < 0 || close(writer))
		failed = 1;
	return failed ? -1 : 0;
}

int main(int argc, char **argv)
{
	struct stat stat_buffer;
	gid_t groups[2] = { 11, 22 };
	int setid_result;
	pid_t child;

	if (argc == 2 && !strcmp(argv[1], "exec"))
		return exec_child();
	if (argc == 2 && !strcmp(argv[1], "setid-exec"))
		return setid_exec_child();
	if (argc == 4 && !strcmp(argv[1], "setid-exec-hold"))
		return setid_exec_hold_child(argv[2], argv[3]);
	if (getuid() || geteuid() || getgid() || getegid())
		return fail("initial ids");
	if ((uid_t)syscall(SYS_setfsuid, (uid_t)-1) != 0 ||
	    (uid_t)syscall(SYS_setfsuid, (uid_t)-1) != 0 ||
	    (gid_t)syscall(SYS_setfsgid, (gid_t)-1) != 0 ||
	    (gid_t)syscall(SYS_setfsgid, (gid_t)-1) != 0)
		return fail("reserved filesystem ids");
	errno = 0;
	if (setuid((uid_t)-1) != -1 || errno != EINVAL ||
	    getuid() || geteuid())
		return fail("reserved uid");
	errno = 0;
	if (setgid((gid_t)-1) != -1 || errno != EINVAL ||
	    getgid() || getegid())
		return fail("reserved gid");
	if (check_filesystem_id_transitions())
		return fail("filesystem id transitions");
	if (setgroups(2, groups) || getgroups(2, groups) != 2 ||
	    groups[0] != 11 || groups[1] != 22)
		return fail("root groups");
	if (umask(0027) != 0022 || create_file(UMASK_FILE, 0777) ||
	    stat(UMASK_FILE, &stat_buffer) ||
	    (stat_buffer.st_mode & 07777) != 0750)
		return fail("umask");
	if (create_file(ROOT_PRIVATE, 0600) ||
	    create_file(ROOT_GROUP, 0640) ||
	    create_file(STICKY_FILE, 0644) ||
	    create_data_file(ROOT_READONLY, 0444) ||
	    create_file(ROOT_WRITABLE, 0666) ||
	    chmod(ROOT_WRITABLE, 0666) || mkdir(PRIVATE_DIR, 0700) ||
	    create_file(PRIVATE_CHILD, 0644) || mkdir(SETGID_DIR, 0777) ||
	    chown(SETGID_DIR, 0, 4321) || chmod(SETGID_DIR, 02777))
		return fail("fixtures");
	child = fork();
	if (child < 0)
		return fail("permission fork");
	if (!child)
		_exit(permission_child(getppid()));
	if (wait_success(child, "permission child"))
		return 1;
	if (stat(OWNED_FILE, &stat_buffer) || stat_buffer.st_uid != 1001 ||
	    stat_buffer.st_gid != 1001 ||
	    (stat_buffer.st_mode & 07777) != 0640)
		return fail("inherited owner");
	if (stat(ROOT_READONLY, &stat_buffer) || stat_buffer.st_size != 4)
		return fail("truncate preserved");
	child = fork();
	if (child < 0)
		return fail("exec fork");
	if (!child) {
		gid_t group = 1001;

		if (setgroups(1, &group) || setresgid(1001, 1001, 1001) ||
		    setresuid(1001, 1001, 1001))
			_exit(fail("exec drop"));
		execl("/bin/credential-runtime", "credential-runtime",
		      "exec", NULL);
		_exit(fail("exec"));
	}
	if (wait_success(child, "exec child"))
		return 1;
	if (chown(CREDENTIAL_PROGRAM, 0, 4321) ||
	    chmod(CREDENTIAL_PROGRAM, 06755))
		return fail("set-ID exec setup");
	child = fork();
	if (child < 0)
		return fail("set-ID exec fork");
	if (!child) {
		gid_t group = 1001;

		if (setgroups(1, &group) || setresgid(1001, 1001, 1001) ||
		    setresuid(1001, 1001, 1001))
			_exit(fail("set-ID exec drop"));
		execl(CREDENTIAL_PROGRAM, "credential-runtime",
		      "setid-exec", NULL);
		_exit(fail("set-ID exec"));
	}
	setid_result = wait_success(child, "set-ID exec child");
	if (!setid_result && check_executable_write_exclusion())
		setid_result = fail("set-ID write exclusion");
	if (chown(CREDENTIAL_PROGRAM, 0, 0) ||
	    chmod(CREDENTIAL_PROGRAM, 0755))
		return fail("set-ID exec restore");
	if (setid_result)
		return 1;
	if (unlink(ROOT_PRIVATE) || unlink(ROOT_GROUP) ||
	    unlink(ROOT_READONLY) || unlink(ROOT_WRITABLE) ||
	    unlink(STICKY_FILE) || unlink(OWNED_FILE) ||
	    unlink(ZERO_MODE_FILE) || unlink(UMASK_FILE) ||
	    unlink(PRIVATE_CHILD) || rmdir(PRIVATE_DIR) ||
	    unlink(SETGID_FILE) || rmdir(SETGID_DIR))
		return fail("cleanup");
	puts("CREDENTIAL_RUNTIME_OK");
	return 0;
}
