#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int signal_fd = -1;
static volatile sig_atomic_t signal_seen;
static volatile sig_atomic_t orphan_cont_seen;
static volatile sig_atomic_t orphan_hup_seen;

static int fail(const char *operation)
{
	printf("PROCESS_GROUP_FAIL %s errno=%d\n", operation, errno);
	return 1;
}

static int read_bytes(int fd, char *buffer, size_t count)
{
	size_t total = 0;

	while (total < count) {
		ssize_t result = read(fd, buffer + total, count - total);

		if (result <= 0)
			return -1;
		total += result;
	}
	return 0;
}

static void user_signal(int signal)
{
	char marker = 'S';

	(void)signal;
	signal_seen = 1;
	if (signal_fd >= 0)
		(void)write(signal_fd, &marker, 1);
}

static void orphan_signal(int signal)
{
	char marker = signal == SIGHUP ? 'H' : 'C';

	if (signal == SIGHUP)
		orphan_hup_seen = 1;
	else if (signal == SIGCONT)
		orphan_cont_seen = 1;
	if (signal_fd >= 0)
		(void)write(signal_fd, &marker, 1);
}

static int read_events(int fd, char *events, size_t count)
{
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	size_t total = 0;

	while (total < count) {
		int result = poll(&poll_fd, 1, 2000);

		if (result != 1 || !(poll_fd.revents & POLLIN))
			return -1;
		result = read(fd, events + total, count - total);
		if (result <= 0)
			return -1;
		total += result;
	}
	return 0;
}

static int test_session(void)
{
	char marker = 'R';
	int ready[2], release[2], status;
	pid_t child;

	if (pipe(ready) || pipe(release))
		return fail("session-pipe");
	child = fork();
	if (child < 0)
		return fail("session-fork");
	if (!child) {
		close(ready[0]);
		close(release[1]);
		if (setsid() != getpid() || getpgrp() != getpid() ||
		    getsid(0) != getpid())
			_exit(10);
		if (write(ready[1], &marker, 1) != 1 ||
		    read(release[0], &marker, 1) != 1)
			_exit(11);
		errno = 0;
		if (setpgid(0, 0) != -1 || errno != EPERM)
			_exit(12);
		_exit(0);
	}
	close(ready[1]);
	close(release[0]);
	if (read_bytes(ready[0], &marker, 1))
		return fail("session-ready");
	if (getpgid(child) != child)
		return fail("session-cross-getpgid");
	if (getsid(child) != child)
		return fail("session-cross-getsid");
	errno = 0;
	if (setpgid(child, getpgrp()) != -1 || errno != EPERM)
		return fail("session-cross-group");
	if (write(release[1], &marker, 1) != 1 ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("session-child");
	close(ready[0]);
	close(release[1]);
	return 0;
}

static void group_child(int pgid, int ready_fd, int event_fd)
{
	struct sigaction action;
	char marker = 'R';

	memset(&action, 0, sizeof(action));
	action.sa_handler = user_signal;
	sigemptyset(&action.sa_mask);
	signal_fd = event_fd;
	if (sigaction(SIGUSR1, &action, NULL) || setpgid(0, pgid) ||
	    write(ready_fd, &marker, 1) != 1)
		_exit(20);
	while (!signal_seen)
		pause();
	_exit(0);
}

static int test_group_signal_and_wait(void)
{
	char markers[2];
	int events[2], ready[2], status;
	pid_t leader, member, waited;

	if (pipe(ready) || pipe(events))
		return fail("group-pipe");
	leader = fork();
	if (leader < 0)
		return fail("group-leader-fork");
	if (!leader) {
		close(ready[0]);
		close(events[0]);
		group_child(0, ready[1], events[1]);
	}
	if (setpgid(leader, leader))
		return fail("group-leader-setpgid");
	member = fork();
	if (member < 0)
		return fail("group-member-fork");
	if (!member) {
		close(ready[0]);
		close(events[0]);
		group_child(leader, ready[1], events[1]);
	}
	if (setpgid(member, leader))
		return fail("group-member-setpgid");
	close(ready[1]);
	close(events[1]);
	if (read_bytes(ready[0], markers, sizeof(markers)) ||
	    getpgid(leader) != leader || getpgid(member) != leader)
		return fail("group-ready");
	if (kill(-leader, SIGUSR1) ||
	    read_bytes(events[0], markers, sizeof(markers)))
		return fail("group-kill");
	waited = waitpid(-leader, &status, 0);
	if ((waited != leader && waited != member) || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("group-wait-first");
	waited = waitpid(-leader, &status, 0);
	if ((waited != leader && waited != member) || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("group-wait-second");
	close(ready[0]);
	close(events[0]);
	return 0;
}

static int test_group_stop(void)
{
	char marker;
	int ready[2], status;
	pid_t child;

	if (pipe(ready))
		return fail("stop-pipe");
	child = fork();
	if (child < 0)
		return fail("stop-fork");
	if (!child) {
		close(ready[0]);
		if (setpgid(0, 0) || write(ready[1], "R", 1) != 1)
			_exit(30);
		for (;;)
			pause();
	}
	close(ready[1]);
	if (setpgid(child, child) || read_bytes(ready[0], &marker, 1))
		return fail("stop-ready");
	if (kill(-child, SIGSTOP) ||
	    waitpid(-child, &status, WUNTRACED) != child ||
	    !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
		return fail("stop-wait");
	if (kill(-child, SIGCONT) ||
	    waitpid(-child, &status, WCONTINUED) != child ||
	    !WIFCONTINUED(status))
		return fail("continue-wait");
	if (kill(-child, SIGTERM) || waitpid(-child, &status, 0) != child ||
	    !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
		return fail("stop-cleanup");
	close(ready[0]);
	return 0;
}

static void orphaned_job(int ready_fd, int event_fd)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = orphan_signal;
	sigemptyset(&action.sa_mask);
	signal_fd = event_fd;
	orphan_cont_seen = 0;
	orphan_hup_seen = 0;
	if (setpgid(0, 0) || sigaction(SIGHUP, &action, NULL) ||
	    sigaction(SIGCONT, &action, NULL) ||
	    write(ready_fd, "R", 1) != 1)
		_exit(31);
	while (!orphan_hup_seen || !orphan_cont_seen)
		pause();
	(void)write(event_fd, "D", 1);
	_exit(0);
}

static int test_orphaned_stopped_group(void)
{
	char events[3], ready_marker;
	int event_pipe[2], job_pipe[2], ready[2], status;
	pid_t job = -1, shell;

	if (pipe(event_pipe) || pipe(job_pipe) || pipe(ready))
		return fail("orphan-pipe");
	shell = fork();
	if (shell < 0)
		return fail("orphan-shell-fork");
	if (!shell) {
		close(event_pipe[0]);
		close(job_pipe[0]);
		if (setsid() < 0)
			_exit(32);
		job = fork();
		if (job < 0)
			_exit(33);
		if (!job)
			orphaned_job(ready[1], event_pipe[1]);
		close(ready[1]);
		if (setpgid(job, job) ||
		    write(job_pipe[1], &job, sizeof(job)) !=
			    (ssize_t)sizeof(job) ||
		    read_bytes(ready[0], &ready_marker, 1) ||
		    kill(-job, SIGSTOP) ||
		    waitpid(job, &status, WUNTRACED) != job ||
		    !WIFSTOPPED(status))
			_exit(34);
		_exit(0);
	}
	close(event_pipe[1]);
	close(job_pipe[1]);
	close(ready[0]);
	close(ready[1]);
	if (read_bytes(job_pipe[0], (char *)&job, sizeof(job)) ||
	    waitpid(shell, &status, 0) != shell || !WIFEXITED(status) ||
	    WEXITSTATUS(status) || read_events(event_pipe[0], events,
						  sizeof(events))) {
		if (job > 0)
			(void)kill(-job, SIGKILL);
		return fail("orphan-result");
	}
	close(event_pipe[0]);
	close(job_pipe[0]);
	if (!memchr(events, 'H', sizeof(events)) ||
	    !memchr(events, 'C', sizeof(events)) ||
	    !memchr(events, 'D', sizeof(events)))
		return fail("orphan-signals");
	return 0;
}

static int test_wait_current_group(void)
{
	int status;
	pid_t child = fork();

	if (child < 0)
		return fail("wait-zero-fork");
	if (!child)
		_exit(37);
	if (waitpid(0, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 37)
		return fail("wait-zero");
	return 0;
}

static int test_zombie_queries(void)
{
	struct sigaction action, previous_action;
	sigset_t blocked, previous_mask, wait_mask;
	int session = getsid(0);
	int status;
	pid_t child;

	memset(&action, 0, sizeof(action));
	action.sa_handler = user_signal;
	sigemptyset(&action.sa_mask);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGCHLD);
	signal_fd = -1;
	signal_seen = 0;
	if (sigaction(SIGCHLD, &action, &previous_action) ||
	    sigprocmask(SIG_BLOCK, &blocked, &previous_mask))
		return fail("zombie-query-setup");
	wait_mask = previous_mask;
	sigdelset(&wait_mask, SIGCHLD);
	child = fork();
	if (child < 0)
		return fail("zombie-query-fork");
	if (!child) {
		if (setpgid(0, 0))
			_exit(1);
		_exit(0);
	}
	while (!signal_seen)
		sigsuspend(&wait_mask);
	if (getpgid(child) != child || getsid(child) != session ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) ||
	    sigprocmask(SIG_SETMASK, &previous_mask, NULL) ||
	    sigaction(SIGCHLD, &previous_action, NULL))
		return fail("zombie-query-result");
	return 0;
}

static int test_exec_restriction(const char *program)
{
	char fd_text[16], marker;
	int ready[2], status;
	pid_t child;

	if (pipe(ready))
		return fail("exec-pipe");
	child = fork();
	if (child < 0)
		return fail("exec-fork");
	if (!child) {
		close(ready[0]);
		snprintf(fd_text, sizeof(fd_text), "%d", ready[1]);
		execl(program, program, "exec-child", fd_text, NULL);
		_exit(40);
	}
	close(ready[1]);
	if (read_bytes(ready[0], &marker, 1))
		return fail("exec-ready");
	errno = 0;
	if (setpgid(child, child) != -1 || errno != EACCES)
		return fail("exec-setpgid");
	if (kill(child, SIGKILL) || waitpid(child, &status, 0) != child ||
	    !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
		return fail("exec-cleanup");
	close(ready[0]);
	return 0;
}

static int test_kill_all_excludes_caller(void)
{
	struct sigaction action;
	char marker;
	int ready[2], status;
	pid_t child;

	memset(&action, 0, sizeof(action));
	action.sa_handler = user_signal;
	sigemptyset(&action.sa_mask);
	signal_fd = -1;
	signal_seen = 0;
	if (sigaction(SIGUSR1, &action, NULL) || pipe(ready) || setuid(1234))
		return fail("kill-all-setup");
	child = fork();
	if (child < 0)
		return fail("kill-all-fork");
	if (!child) {
		close(ready[0]);
		if (write(ready[1], "R", 1) != 1)
			_exit(60);
		while (!signal_seen)
			pause();
		_exit(0);
	}
	close(ready[1]);
	if (read_bytes(ready[0], &marker, 1) || kill(-1, SIGUSR1) ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("kill-all-child");
	if (signal_seen)
		return fail("kill-all-caller");
	close(ready[0]);
	return 0;
}

int main(int argc, char **argv)
{
	char marker = 'R';
	int fd;

	if (argc == 3 && !strcmp(argv[1], "exec-child")) {
		fd = atoi(argv[2]);
		if (write(fd, &marker, 1) != 1)
			return 50;
		for (;;)
			pause();
	}
	if (argc != 1)
		return fail("arguments");
	if (getpgid(0) != getpgrp() || getsid(0) <= 0)
		return fail("identity");
	errno = 0;
	if (setpgid(-1, 0) != -1 || errno != EINVAL)
		return fail("negative-pid");
	errno = 0;
	if (kill(-2147483647, 0) != -1 || errno != ESRCH)
		return fail("missing-group");
	if (test_session() || test_group_signal_and_wait() ||
	    test_group_stop() || test_orphaned_stopped_group() ||
	    test_wait_current_group() ||
	    test_zombie_queries() ||
	    test_exec_restriction(argv[0]) ||
	    test_kill_all_excludes_caller())
		return 1;
	puts("PROCESS_GROUP_OK");
	return 0;
}
