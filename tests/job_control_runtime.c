#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int fail(const char *operation)
{
	char message[96];
	int length = snprintf(message, sizeof(message),
	                      "JOB_CONTROL_FAIL %s errno=%d\n",
	                      operation, errno);

	(void)write(1, message, length);
	return 1;
}

static void continued(int signal)
{
	static const char message[] = "JOB_CONTINUED\n";

	(void)signal;
	(void)write(1, message, sizeof(message) - 1);
}

static int check_foreground(void)
{
	if (!isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) != getpgrp())
		return fail("foreground-group");
	return 0;
}

static int wait_forever(const char *marker, int handle_continue)
{
	struct sigaction action;

	if (check_foreground())
		return 1;
	if (handle_continue) {
		memset(&action, 0, sizeof(action));
		action.sa_handler = continued;
		action.sa_flags = SA_RESTART;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGCONT, &action, NULL))
			return fail("sigcont-handler");
	}
	if (write(1, marker, strlen(marker)) != (ssize_t)strlen(marker))
		return fail("ready-write");
	for (;;)
		pause();
}

static int background_read(void)
{
	static const char ready[] = "JOB_BACKGROUND_READ_READY\n";
	static const char expected[] = "job-input\n";
	struct sigaction action;
	char buffer[32];
	ssize_t count;

	memset(&action, 0, sizeof(action));
	action.sa_handler = continued;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCONT, &action, NULL))
		return fail("read-sigcont-handler");
	if (write(1, ready, sizeof(ready) - 1) != sizeof(ready) - 1)
		return fail("read-ready-write");
	count = read(STDIN_FILENO, buffer, sizeof(buffer));
	if (count != sizeof(expected) - 1 ||
	    memcmp(buffer, expected, sizeof(expected) - 1))
		return fail("background-read");
	if (write(1, "JOB_BACKGROUND_READ_OK\n", 23) != 23)
		return fail("read-result-write");
	return 0;
}

static int background_ioctl(int attributes)
{
	static const char pgrp_ready[] = "JOB_PGRP_IOCTL_READY\n";
	static const char pgrp_ok[] = "JOB_PGRP_IOCTL_OK\n";
	static const char termios_ready[] = "JOB_TERMIOS_IOCTL_READY\n";
	static const char termios_ok[] = "JOB_TERMIOS_IOCTL_OK\n";
	struct sigaction action;
	struct termios termios;
	const char *ready = attributes ? termios_ready : pgrp_ready;
	const char *result = attributes ? termios_ok : pgrp_ok;

	memset(&action, 0, sizeof(action));
	action.sa_handler = continued;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCONT, &action, NULL))
		return fail("ioctl-sigcont-handler");
	if (write(STDOUT_FILENO, ready, strlen(ready)) !=
	    (ssize_t)strlen(ready))
		return fail("ioctl-ready-write");
	if (attributes) {
		if (tcgetattr(STDIN_FILENO, &termios))
			return fail("background-termios-get");
		termios.c_lflag |= ISIG | ICANON | ECHO;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &termios))
			return fail("background-termios-ioctl");
	} else if (tcsetpgrp(STDIN_FILENO, getpgrp())) {
		return fail("background-pgrp-ioctl");
	}
	if (write(STDOUT_FILENO, result, strlen(result)) !=
	    (ssize_t)strlen(result))
		return fail("ioctl-result-write");
	return 0;
}

struct orphan_io_result {
	ssize_t count;
	int error;
};

static int orphaned_background_io(int writing)
{
	static const char read_marker[] = "JOB_ORPHAN_READ_OK\n";
	static const char write_marker[] = "JOB_ORPHAN_WRITE_OK\n";
	struct orphan_io_result result;
	struct pollfd pollfd;
	struct termios original, tostop;
	const char *failure = 0;
	const char *marker;
	size_t marker_length;
	int terminal_changed = 0;
	int pid_pipe[2], result_pipe[2], start_pipe[2], status;
	pid_t intermediate, reader;
	char byte;

	if (pipe(pid_pipe) || pipe(result_pipe) || pipe(start_pipe))
		return fail("orphan-pipe");
	intermediate = fork();
	if (intermediate < 0)
		return fail("orphan-fork");
	if (!intermediate) {
		close(pid_pipe[0]);
		close(result_pipe[0]);
		close(start_pipe[1]);
		reader = fork();
		if (reader < 0)
			_exit(1);
		if (reader) {
			close(result_pipe[1]);
			close(start_pipe[0]);
			if (write(pid_pipe[1], &reader, sizeof(reader)) !=
			    sizeof(reader))
				_exit(1);
			close(pid_pipe[1]);
			_exit(0);
		}
		close(pid_pipe[1]);
		if (setpgid(0, 0) || read(start_pipe[0], &byte, 1) != 1)
			_exit(1);
		close(start_pipe[0]);
		errno = 0;
		result.count = writing ? write(STDOUT_FILENO, &byte, 1) :
			read(STDIN_FILENO, &byte, 1);
		result.error = errno;
		if (write(result_pipe[1], &result, sizeof(result)) !=
		    sizeof(result))
			_exit(1);
		close(result_pipe[1]);
		_exit(result.count == -1 && result.error == EIO ? 0 : 1);
	}
	close(pid_pipe[1]);
	close(result_pipe[1]);
	close(start_pipe[0]);
	if (read(pid_pipe[0], &reader, sizeof(reader)) != sizeof(reader) ||
	    waitpid(intermediate, &status, 0) != intermediate ||
	    !WIFEXITED(status) || WEXITSTATUS(status))
		return fail("orphan-setup");
	if (writing) {
		if (tcgetattr(STDOUT_FILENO, &original))
			failure = "orphan-write-getattr";
		else {
			tostop = original;
			tostop.c_lflag |= TOSTOP;
			if (tcsetattr(STDOUT_FILENO, TCSANOW, &tostop))
				failure = "orphan-write-setattr";
			else
				terminal_changed = 1;
		}
	}
	if (!failure && write(start_pipe[1], "R", 1) != 1)
		failure = "orphan-start";
	close(pid_pipe[0]);
	close(start_pipe[1]);
	if (!failure) {
		pollfd.fd = result_pipe[0];
		pollfd.events = POLLIN;
		pollfd.revents = 0;
		if (poll(&pollfd, 1, 1000) != 1 ||
		    !(pollfd.revents & POLLIN))
			failure = writing ? "orphan-write-timeout" :
				"orphan-read-timeout";
	}
	if (!failure &&
	    (read(result_pipe[0], &result, sizeof(result)) != sizeof(result) ||
	     result.count != -1 || result.error != EIO))
		failure = writing ? "orphan-write-result" :
			"orphan-read-result";
	close(result_pipe[0]);
	if (failure)
		kill(reader, SIGKILL);
	if (terminal_changed &&
	    tcsetattr(STDOUT_FILENO, TCSANOW, &original))
		return fail("orphan-write-restore");
	if (failure)
		return fail(failure);
	marker = writing ? write_marker : read_marker;
	marker_length = strlen(marker);
	if (write(STDOUT_FILENO, marker, marker_length) !=
	    (ssize_t)marker_length)
		return fail("orphan-result-write");
	return 0;
}

static void hung_up(int signal)
{
	static const char message[] = "JOB_HANGUP_OK\n";

	if (signal == SIGHUP)
		(void)write(STDOUT_FILENO, message, sizeof(message) - 1);
	_exit(signal == SIGHUP ? 0 : 1);
}

static int check_session_hangup(void)
{
	struct sigaction action;
	char marker;
	int status;
	int ready[2];
	pid_t child;

	if (getsid(0) != getpid())
		return fail("hangup-session-leader");
	if (pipe(ready))
		return fail("hangup-pipe");
	child = fork();
	if (child < 0)
		return fail("hangup-fork");
	if (!child) {
		close(ready[0]);
		memset(&action, 0, sizeof(action));
		action.sa_handler = hung_up;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGHUP, &action, NULL) || setpgid(0, 0) ||
		    write(ready[1], "R", 1) != 1)
			_exit(1);
		for (;;)
			pause();
	}
	close(ready[1]);
	if (read(ready[0], &marker, 1) != 1 || marker != 'R')
		return fail("hangup-ready");
	if (tcsetpgrp(STDIN_FILENO, child))
		return fail("hangup-foreground");
	if (kill(-child, SIGSTOP) ||
	    waitpid(child, &status, WUNTRACED) != child ||
	    !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
		return fail("hangup-stop");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return fail("arguments");
	if (!strcmp(argv[1], "foreground"))
		return wait_forever("JOB_FOREGROUND_READY\n", 0);
	if (!strcmp(argv[1], "stop"))
		return wait_forever("JOB_STOP_READY\n", 1);
	if (!strcmp(argv[1], "read"))
		return background_read();
	if (!strcmp(argv[1], "ioctl-pgrp"))
		return background_ioctl(0);
	if (!strcmp(argv[1], "ioctl-termios"))
		return background_ioctl(1);
	if (!strcmp(argv[1], "orphan-read"))
		return orphaned_background_io(0);
	if (!strcmp(argv[1], "orphan-write"))
		return orphaned_background_io(1);
	if (!strcmp(argv[1], "hangup"))
		return check_session_hangup();
	return fail("mode");
}
