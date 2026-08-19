#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIXTURE_ADDRESS "10.0.2.2"
#define UDP_PORT 18080
#define TCP_PORT 18081
#define TCP_LISTEN_PORT 18082
#define TCP_BULK_PORT 18083
#define LOOPBACK_PORT 18084
#define LOOPBACK_POLL_PORT 18085
#define TCP_LINGER_PORT 18086
#define TCP_BULK_SIZE 32768
#define LARGE_RECEIVE_SIZE 8192
#define UDP_READV_SIZE 8192
#define UDP_OVERSIZE_SIZE 4097
#define DEFAULT_IP_TTL 255
#define MAX_LWIP_LINGER 32767
#define TEST_IOV_MAX 1024

struct icmp_echo {
	unsigned char type;
	unsigned char code;
	unsigned short checksum;
	unsigned short identifier;
	unsigned short sequence;
	unsigned char payload[24];
};

static unsigned char bulk_send[TCP_BULK_SIZE];
static unsigned char bulk_receive[TCP_BULK_SIZE];
static unsigned char large_receive[LARGE_RECEIVE_SIZE];
static unsigned char udp_readv_receive[UDP_READV_SIZE];
static unsigned char udp_oversize[UDP_OVERSIZE_SIZE];
static volatile sig_atomic_t sigpipe_seen;

static void sigpipe_handler(int signal)
{
	(void)signal;
	sigpipe_seen++;
}

static unsigned short internet_checksum(const void *buffer, size_t length)
{
	const unsigned short *words = buffer;
	unsigned int sum = 0;

	while (length > 1) {
		sum += *words++;
		length -= 2;
	}
	if (length)
		sum += *(const unsigned char *)words;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static int fail(const char *operation)
{
	printf("NETWORK_RUNTIME_FAIL %s errno=%d\n", operation, errno);
	return 1;
}

static int wait_ready(int fd, short events)
{
	struct pollfd pollfd = {
		.fd = fd,
		.events = events,
	};

	if (poll(&pollfd, 1, 3000) != 1)
		return -1;
	return pollfd.revents & events ? 0 : -1;
}

static int wait_readable(int fd)
{
	return wait_ready(fd, POLLIN);
}

static int wait_writable(int fd)
{
	return wait_ready(fd, POLLOUT);
}

static int sendto_when_ready(int fd, const void *buffer, size_t length,
			     const struct sockaddr_in *peer)
{
	struct timespec start, now;

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
		return -1;
	for (;;) {
		if (sendto(fd, buffer, length, 0,
			   (const struct sockaddr *)peer, sizeof(*peer)) ==
		    (ssize_t)length)
			return 0;
		if (errno != EHOSTUNREACH && errno != ENETUNREACH &&
		    errno != EADDRNOTAVAIL)
			return -1;
		if (poll(0, 0, 50) < 0 ||
		    clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		if (now.tv_sec - start.tv_sec >= 5)
			return -1;
	}
}

static int write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	size_t written = 0;

	while (written < length) {
		ssize_t result = write(fd, bytes + written, length - written);

		if (result < 0 && errno == EAGAIN) {
			if (wait_writable(fd) < 0)
				return -1;
			continue;
		}
		if (result <= 0)
			return -1;
		written += result;
	}
	return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
	unsigned char *bytes = buffer;
	size_t received = 0;

	while (received < length) {
		ssize_t result = read(fd, bytes + received, length - received);

		if (result < 0 && errno == EAGAIN) {
			if (wait_readable(fd) < 0)
				return -1;
			continue;
		}
		if (result <= 0)
			return -1;
		received += result;
	}
	return 0;
}

static int udp_test(const struct sockaddr_in *host)
{
	struct iovec iovecs[2];
	struct sockaddr_in peer;
	socklen_t peer_length = sizeof(peer);
	char reply[32];
	int fd, flags, index;
	ssize_t length;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fail("udp socket");
	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || !(flags & FD_CLOEXEC))
		return fail("udp cloexec");
	if (sendto(fd, "udp-request", 11, 0,
		   (const struct sockaddr *)host, sizeof(*host)) != 11)
		return fail("udp sendto");
	if (wait_readable(fd) < 0)
		return fail("udp poll");
	length = recvfrom(fd, reply, sizeof(reply), 0,
			  (struct sockaddr *)&peer, &peer_length);
	if (length != 9 || memcmp(reply, "udp-reply", 9))
		return fail("udp recvfrom");
	if (peer.sin_family != AF_INET ||
	    ntohs(peer.sin_port) != UDP_PORT)
		return fail("udp peer");
	if (sendto(fd, "udp-readv-large", 15, 0,
		   (const struct sockaddr *)host, sizeof(*host)) != 15 ||
	    wait_readable(fd) < 0)
		return fail("large UDP readv setup");
	iovecs[0].iov_base = udp_readv_receive;
	iovecs[0].iov_len = 3072;
	iovecs[1].iov_base = udp_readv_receive + 3072;
	iovecs[1].iov_len = sizeof(udp_readv_receive) - 3072;
	if (readv(fd, iovecs, 2) != sizeof(udp_readv_receive))
		return fail("large UDP readv length");
	for (index = 0; index < (int)sizeof(udp_readv_receive); index++) {
		if (udp_readv_receive[index] != (unsigned char)index)
			return fail("large UDP readv contents");
	}
	if (close(fd) < 0)
		return fail("udp close");
	return 0;
}

static int loopback_test(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(LOOPBACK_PORT),
	};
	struct sockaddr_in disconnect = {
		.sin_family = AF_UNSPEC,
	};
	struct timeval timeout = {
		.tv_usec = 1,
	};
	struct timeval timeout_result;
	unsigned char guarded[12];
	unsigned char control[32];
	char reply[32];
	struct iovec iovecs[2];
	struct msghdr message;
	struct iovec *maximum_iovecs;
	struct pollfd pollfd;
	socklen_t peer_length;
	struct sockaddr_in peer;
	int index, receive, receive_buffer, transmit;
	ssize_t length;

	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return fail("loopback address");
	receive = socket(AF_INET, SOCK_DGRAM, 0);
	transmit = socket(AF_INET, SOCK_DGRAM, 0);
	if (receive < 0 || transmit < 0 ||
	    bind(receive, (const struct sockaddr *)&address,
		 sizeof(address)) < 0)
		return fail("loopback setup");
	if (setsockopt(receive, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		       sizeof(timeout)) < 0 ||
	    setsockopt(receive, SOL_SOCKET, SO_SNDTIMEO, &timeout,
		       sizeof(timeout)) < 0)
		return fail("sub-millisecond timeout set");
	peer_length = sizeof(timeout_result);
	memset(&timeout_result, 0, sizeof(timeout_result));
	if (getsockopt(receive, SOL_SOCKET, SO_RCVTIMEO, &timeout_result,
		       &peer_length) < 0 || timeout_result.tv_sec ||
	    timeout_result.tv_usec < 1000)
		return fail("sub-millisecond receive timeout get");
	peer_length = sizeof(timeout_result);
	memset(&timeout_result, 0, sizeof(timeout_result));
	if (getsockopt(receive, SOL_SOCKET, SO_SNDTIMEO, &timeout_result,
		       &peer_length) < 0 || timeout_result.tv_sec ||
	    timeout_result.tv_usec < 1000)
		return fail("sub-millisecond send timeout get");
	errno = 0;
	if (recv(receive, reply, 1, 0) != -1 || errno != EAGAIN)
		return fail("sub-millisecond receive timeout");
	receive_buffer = 1;
	if (setsockopt(receive, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
		       sizeof(receive_buffer)) < 0)
		return fail("small receive buffer set");
	peer_length = sizeof(receive_buffer);
	if (getsockopt(receive, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
		       &peer_length) < 0 || receive_buffer < 2304)
		return fail("small receive buffer clamp");
	errno = 0;
	if (sendto(transmit, "more", 4, MSG_MORE,
		   (const struct sockaddr *)&address, sizeof(address)) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("UDP MSG_MORE rejection");
	errno = 0;
	if (recv(receive, reply, 1, MSG_WAITALL | MSG_DONTWAIT) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("MSG_WAITALL rejection");
	if (sendto(transmit, "discard", 7, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 7 ||
	    wait_readable(receive) < 0)
		return fail("zero-length datagram setup");
	peer_length = sizeof(peer);
	if (recvfrom(receive, reply, 0, 0, (struct sockaddr *)&peer,
		     &peer_length) != 0 || peer.sin_family != AF_INET)
		return fail("zero-length datagram");
	receive_buffer = -1;
	if (setsockopt(receive, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
		       sizeof(receive_buffer)) < 0)
		return fail("negative receive buffer set");
	peer_length = sizeof(receive_buffer);
	if (getsockopt(receive, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
		       &peer_length) < 0 || receive_buffer <= 2304)
		return fail("negative receive buffer clamp");
	if (sendto(transmit, "loopback", 8, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 8 ||
	    wait_readable(receive) < 0 ||
	    recv(receive, reply, sizeof(reply), 0) != 8 ||
	    memcmp(reply, "loopback", 8))
		return fail("loopback UDP");
	if (sendto(transmit, "truncate", 8, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 8 ||
	    wait_readable(receive) < 0)
		return fail("recvmsg setup");
	memset(&message, 0, sizeof(message));
	memset(&peer, 0, sizeof(peer));
	iovecs[0].iov_base = reply;
	iovecs[0].iov_len = 1;
	iovecs[1].iov_base = reply + 1;
	iovecs[1].iov_len = 2;
	message.msg_name = &peer;
	message.msg_namelen = sizeof(peer);
	message.msg_iov = iovecs;
	message.msg_iovlen = 2;
	length = recvmsg(receive, &message, 0);
	if (length != 3 || memcmp(reply, "tru", 3) ||
	    !(message.msg_flags & MSG_TRUNC) || peer.sin_family != AF_INET)
		return fail("recvmsg truncation");
	if (sendto(transmit, "truncate", 8, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 8 ||
	    wait_readable(receive) < 0)
		return fail("recvmsg MSG_TRUNC setup");
	memset(&message, 0, sizeof(message));
	memset(&peer, 0, sizeof(peer));
	memset(control, 0xa5, sizeof(control));
	iovecs[0].iov_base = reply;
	iovecs[0].iov_len = 1;
	iovecs[1].iov_base = reply + 1;
	iovecs[1].iov_len = 2;
	message.msg_name = &peer;
	message.msg_namelen = sizeof(peer);
	message.msg_iov = iovecs;
	message.msg_iovlen = 2;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	length = recvmsg(receive, &message, MSG_TRUNC);
	if (length != 8 || memcmp(reply, "tru", 3) ||
	    !(message.msg_flags & MSG_TRUNC) || message.msg_controllen ||
	    peer.sin_family != AF_INET)
		return fail("recvmsg MSG_TRUNC");
	if (sendto(transmit, "truncate", 8, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 8 ||
	    wait_readable(receive) < 0)
		return fail("recvfrom MSG_TRUNC setup");
	memset(guarded, 0xa5, sizeof(guarded));
	length = recvfrom(receive, guarded + 1, 3, MSG_TRUNC, 0, 0);
	if (length != 8 || memcmp(guarded + 1, "tru", 3) ||
	    guarded[0] != 0xa5)
		return fail("recvfrom MSG_TRUNC");
	for (index = 4; index < (int)sizeof(guarded); index++) {
		if (guarded[index] != 0xa5)
			return fail("recvfrom MSG_TRUNC overwrite");
	}
	if (sendto(transmit, "truncate", 8, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 8 ||
	    wait_readable(receive) < 0 ||
	    recvfrom(receive, 0, 0, MSG_TRUNC, 0, 0) != 8)
		return fail("zero-capacity recvfrom MSG_TRUNC");
	if (sendto(transmit, "large", 5, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 5 ||
	    wait_readable(receive) < 0 ||
	    recvfrom(receive, large_receive, sizeof(large_receive), 0,
		     0, 0) != 5 || memcmp(large_receive, "large", 5))
		return fail("large receive capacity");
	if (connect(transmit, (const struct sockaddr *)&address,
		    sizeof(address)) < 0)
		return fail("UDP connect");
	if (write(transmit, "scatter", 7) != 7 ||
	    write(transmit, "next", 4) != 4 || wait_readable(receive) < 0)
		return fail("UDP readv setup");
	memset(reply, 0, sizeof(reply));
	iovecs[0].iov_base = reply;
	iovecs[0].iov_len = 4;
	iovecs[1].iov_base = reply + 4;
	iovecs[1].iov_len = 5;
	if (readv(receive, iovecs, 2) != 7 ||
	    memcmp(reply, "scatter", 7) ||
	    recv(receive, reply, sizeof(reply), 0) != 4 ||
	    memcmp(reply, "next", 4))
		return fail("UDP readv datagram");
	maximum_iovecs = calloc(TEST_IOV_MAX, sizeof(*maximum_iovecs));
	if (!maximum_iovecs)
		return fail("maximum iovec allocation");
	memset(&message, 0, sizeof(message));
	message.msg_iov = maximum_iovecs;
	message.msg_iovlen = TEST_IOV_MAX;
	if (sendmsg(transmit, &message, 0) != 0 ||
	    wait_readable(receive) < 0 ||
	    recvmsg(receive, &message, 0) != 0)
		return fail("maximum iovec message");
	free(maximum_iovecs);
	memset(&message, 0, sizeof(message));
	iovecs[0].iov_base = reply;
	iovecs[0].iov_len = 0;
	iovecs[1].iov_base = reply;
	iovecs[1].iov_len = 0;
	message.msg_iov = iovecs;
	message.msg_iovlen = 2;
	if (sendmsg(transmit, &message, 0) != 0 ||
	    wait_readable(receive) < 0 ||
	    recv(receive, reply, sizeof(reply), 0) != 0)
		return fail("zero-length UDP sendmsg");
	if (write(transmit, reply, 0) != 0 || wait_readable(receive) < 0 ||
	    recv(receive, reply, sizeof(reply), 0) != 0)
		return fail("zero-length UDP write");
	iovecs[0].iov_base = "vector-";
	iovecs[0].iov_len = 7;
	iovecs[1].iov_base = "udp";
	iovecs[1].iov_len = 3;
	if (writev(transmit, iovecs, 2) != 10 ||
	    wait_readable(receive) < 0 ||
	    recv(receive, reply, sizeof(reply), 0) != 10 ||
	    memcmp(reply, "vector-udp", 10))
		return fail("UDP writev datagram");
	iovecs[0].iov_len = 0;
	iovecs[1].iov_len = 0;
	pollfd.fd = receive;
	pollfd.events = POLLIN;
	pollfd.revents = 0;
	if (writev(transmit, iovecs, 2) != 0 || poll(&pollfd, 1, 50) != 0)
		return fail("zero-length UDP writev");
	if (connect(transmit, (const struct sockaddr *)&disconnect,
		    sizeof(disconnect)) < 0)
		return fail("UDP disconnect");
	errno = 0;
	if (write(transmit, "x", 1) != -1 || errno != EDESTADDRREQ)
		return fail("UDP disconnected write");
	if (sendto(transmit, "again", 5, 0,
		   (const struct sockaddr *)&address, sizeof(address)) != 5 ||
	    wait_readable(receive) < 0 ||
	    recv(receive, reply, sizeof(reply), 0) != 5 ||
	    memcmp(reply, "again", 5))
		return fail("UDP sendto after disconnect");
	if (connect(transmit, (const struct sockaddr *)&address,
		    sizeof(address)) < 0)
		return fail("UDP reconnect");
	errno = 0;
	if (write(transmit, udp_oversize, sizeof(udp_oversize)) != -1 ||
	    errno != EMSGSIZE)
		return fail("oversized UDP write");
	if (shutdown(transmit, SHUT_RDWR) < 0)
		return fail("UDP shutdown");
	errno = 0;
	if (send(transmit, "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("UDP send after shutdown");
	if (recv(transmit, reply, sizeof(reply), MSG_DONTWAIT) != 0)
		return fail("UDP receive after shutdown");
	if (close(transmit) < 0 || close(receive) < 0)
		return fail("loopback close");
	return 0;
}

static int inherited_poll_test(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(LOOPBACK_POLL_PORT),
	};
	char reply[8];
	int fd, status;
	pid_t child;

	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return fail("poll address");
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || bind(fd, (const struct sockaddr *)&address,
			  sizeof(address)) < 0)
		return fail("poll setup");
	child = fork();
	if (child < 0)
		return fail("poll fork");
	if (!child) {
		int result;

		poll(0, 0, 50);
		result = sendto(fd, "wake", 4, 0,
			(const struct sockaddr *)&address, sizeof(address)) == 4 ?
			0 : 1;
		close(fd);
		_exit(result);
	}
	if (wait_readable(fd) < 0 || recv(fd, reply, sizeof(reply), 0) != 4 ||
	    memcmp(reply, "wake", 4))
		return fail("inherited socket poll");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return fail("poll child");
	if (close(fd) < 0)
		return fail("poll close");
	return 0;
}

static int protocol_test(void)
{
	static const struct {
		int type;
		int protocol;
	} invalid[] = {
		{ SOCK_STREAM, IPPROTO_UDP },
		{ SOCK_DGRAM, IPPROTO_TCP },
		{ SOCK_RAW, 0 },
	};
	int fd;
	int pair[2];
	size_t index;

	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		errno = 0;
		fd = socket(AF_INET, invalid[index].type,
			    invalid[index].protocol);
		if (fd >= 0) {
			close(fd);
			return fail("protocol accepted");
		}
		if (errno != EPROTONOSUPPORT)
			return fail("protocol error");
	}
	errno = 0;
	if (socketpair(AF_INET, SOCK_STREAM, 0, pair) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("IPv4 socketpair error");
	errno = 0;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != -1 ||
	    errno != EAFNOSUPPORT)
		return fail("unsupported socketpair family");
	return 0;
}

static int sigpipe_test(void)
{
	struct sigaction action;
	struct iovec iovec = {
		.iov_base = (void *)"x",
		.iov_len = 1,
	};
	struct msghdr message = {
		.msg_iov = &iovec,
		.msg_iovlen = 1,
	};
	int fd;

	memset(&action, 0, sizeof(action));
	action.sa_handler = sigpipe_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPIPE, &action, NULL))
		return fail("SIGPIPE action");
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return fail("SIGPIPE socket");
	sigpipe_seen = 0;
	errno = 0;
	if (send(fd, "x", 1, 0) != -1 || errno != EPIPE ||
	    sigpipe_seen != 1)
		return fail("send SIGPIPE");
	sigpipe_seen = 0;
	errno = 0;
	if (send(fd, "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE ||
	    sigpipe_seen)
		return fail("MSG_NOSIGNAL");
	sigpipe_seen = 0;
	errno = 0;
	if (write(fd, "x", 1) != -1 || errno != EPIPE || sigpipe_seen != 1)
		return fail("write SIGPIPE");
	sigpipe_seen = 0;
	errno = 0;
	if (writev(fd, &iovec, 1) != -1 || errno != EPIPE ||
	    sigpipe_seen != 1)
		return fail("writev SIGPIPE");
	sigpipe_seen = 0;
	errno = 0;
	if (sendmsg(fd, &message, 0) != -1 || errno != EPIPE ||
	    sigpipe_seen != 1)
		return fail("sendmsg SIGPIPE");
	if (close(fd))
		return fail("SIGPIPE close");
	return 0;
}

static int broadcast_permission_test(void)
{
	struct sockaddr_in broadcast = {
		.sin_family = AF_INET,
		.sin_port = htons(UDP_PORT),
	};
	socklen_t length;
	int enabled, fd, raw;

	if (inet_pton(AF_INET, "255.255.255.255", &broadcast.sin_addr) != 1)
		return fail("broadcast address");
	raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (raw < 0)
		return fail("raw broadcast socket");
	errno = 0;
	if (sendto(raw, "x", 1, 0,
		   (const struct sockaddr *)&broadcast,
		   sizeof(broadcast)) != -1 || errno != EACCES)
		return fail("raw broadcast permission");
	if (close(raw) < 0)
		return fail("raw broadcast close");
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return fail("broadcast socket");
	errno = 0;
	if (sendto(fd, "x", 1, 0, (const struct sockaddr *)&broadcast,
		   sizeof(broadcast)) != -1 || errno != EACCES)
		return fail("broadcast permission");
	errno = 0;
	if (connect(fd, (const struct sockaddr *)&broadcast,
		    sizeof(broadcast)) != -1 || errno != EACCES)
		return fail("broadcast connect permission");
	enabled = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled,
		       sizeof(enabled)) < 0)
		return fail("broadcast enable");
	length = sizeof(enabled);
	enabled = 0;
	if (getsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, &length) < 0 ||
	    !enabled)
		return fail("broadcast option");
	if (connect(fd, (const struct sockaddr *)&broadcast,
		    sizeof(broadcast)) < 0)
		return fail("broadcast connect enabled");
	broadcast.sin_family = AF_UNSPEC;
	if (connect(fd, (const struct sockaddr *)&broadcast,
		    sizeof(broadcast)) < 0)
		return fail("broadcast disconnect");
	broadcast.sin_family = AF_INET;
	enabled = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled,
		       sizeof(enabled)) < 0)
		return fail("broadcast disable");
	errno = 0;
	if (sendto(fd, "x", 1, 0, (const struct sockaddr *)&broadcast,
		   sizeof(broadcast)) != -1 || errno != EACCES)
		return fail("broadcast permission after disable");
	if (close(fd) < 0)
		return fail("broadcast close");
	return 0;
}

static int icmp_test(const struct sockaddr_in *host)
{
	struct icmp_echo request = {
		.type = 8,
		.identifier = htons(0x4341),
		.sequence = htons(1),
	};
	struct sockaddr_in peer;
	socklen_t peer_length = sizeof(peer);
	unsigned char reply[256];
	int fd, index;
	ssize_t length;

	for (index = 0; index < (int)sizeof(request.payload); index++)
		request.payload[index] = index * 19 + 7;
	request.checksum = internet_checksum(&request, sizeof(request));
	fd = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP);
	if (fd < 0)
		return fail("ICMP socket");
	errno = 0;
	if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != -1 ||
	    errno != EDESTADDRREQ)
		return fail("unconnected ICMP send");
	if (sendto_when_ready(fd, &request, sizeof(request), host) < 0 ||
	    wait_readable(fd) < 0)
		return fail("ICMP send");
	length = recvfrom(fd, reply, sizeof(reply), 0,
			  (struct sockaddr *)&peer, &peer_length);
	if (length < 8)
		return fail("ICMP receive");
	if (peer.sin_family != AF_INET || peer.sin_port)
		return fail("ICMP peer address");
	for (index = 0; index + 8 <= length; index++) {
		if (!reply[index] && !reply[index + 1] &&
		    !memcmp(reply + index + 4, &request.identifier, 2) &&
		    !memcmp(reply + index + 6, &request.sequence, 2))
			break;
	}
	if (index + 8 > length)
		return fail("ICMP reply");
	if (connect(fd, (const struct sockaddr *)host, sizeof(*host)) < 0)
		return fail("ICMP connect");
	peer = *host;
	peer.sin_family = AF_UNSPEC;
	if (connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0)
		return fail("ICMP disconnect");
	errno = 0;
	if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != -1 ||
	    errno != EDESTADDRREQ)
		return fail("ICMP send after disconnect");
	if (connect(fd, (const struct sockaddr *)host, sizeof(*host)) < 0)
		return fail("ICMP reconnect");
	if (shutdown(fd, SHUT_RDWR) < 0)
		return fail("ICMP shutdown");
	errno = 0;
	if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != -1 ||
	    errno != EPIPE)
		return fail("ICMP send after shutdown");
	if (recv(fd, reply, sizeof(reply), MSG_DONTWAIT) != 0)
		return fail("ICMP receive after shutdown");
	if (close(fd) < 0)
		return fail("ICMP close");
	return 0;
}

static int tcp_test(const struct sockaddr_in *host)
{
	const int invalid_ttl[] = { 0, 256, -2 };
	union {
		int value;
		unsigned char bytes[32];
	} option;
	struct linger linger = {
		.l_onoff = 1,
		.l_linger = 1,
	};
	struct linger linger_result;
	struct iovec fault_iovecs[2];
	struct iovec iovec;
	struct msghdr message;
	struct sockaddr_in peer;
	struct stat stat;
	char reply[32];
	int available, error, fd, index;
	socklen_t length, peer_length;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return fail("tcp socket");
	if (fstat(fd, &stat) < 0 || !S_ISSOCK(stat.st_mode))
		return fail("tcp fstat");
	errno = 0;
	if (lseek(fd, 0, SEEK_END) != -1 || errno != ESPIPE)
		return fail("tcp lseek");
	errno = 0;
	if (syscall(SYS_getdents64, fd, reply, sizeof(reply)) != -1 ||
	    errno != ENOTDIR)
		return fail("tcp getdents");
	errno = 0;
	if (send(fd, "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("unconnected TCP send");
	errno = 0;
	if (send(fd, "", 0, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("zero-length unconnected TCP send");
	memset(&message, 0, sizeof(message));
	iovec.iov_base = reply;
	iovec.iov_len = 0;
	message.msg_iov = &iovec;
	message.msg_iovlen = 1;
	errno = 0;
	if (sendmsg(fd, &message, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("unconnected TCP sendmsg");
	errno = 0;
	if (write(fd, "", 0) != -1 || errno != EPIPE)
		return fail("zero-length unconnected TCP write");
	if (writev(fd, &iovec, 1) != 0)
		return fail("zero-length unconnected TCP writev");
	iovec.iov_len = 1;
	errno = 0;
	if (writev(fd, &iovec, 1) != -1 || errno != EPIPE)
		return fail("unconnected TCP writev");
	if (fcntl(fd, F_SETFL, O_APPEND) < 0)
		return fail("tcp append flag");
	if (connect(fd, (const struct sockaddr *)host, sizeof(*host)) < 0)
		return fail("tcp connect");
	memset(&peer, 0xa5, sizeof(peer));
	peer_length = sizeof(peer);
	if (recvfrom(fd, reply, 0, 0, (struct sockaddr *)&peer,
		     &peer_length) != 0 || peer_length != sizeof(peer) ||
	    peer.sin_family != AF_INET || peer.sin_port != host->sin_port ||
	    peer.sin_addr.s_addr != host->sin_addr.s_addr)
		return fail("zero-length TCP recvfrom peer");
	memset(&message, 0, sizeof(message));
	memset(&peer, 0xa5, sizeof(peer));
	message.msg_name = &peer;
	message.msg_namelen = sizeof(peer);
	if (recvmsg(fd, &message, 0) != 0 ||
	    message.msg_namelen != sizeof(peer) ||
	    peer.sin_family != AF_INET || peer.sin_port != host->sin_port ||
	    peer.sin_addr.s_addr != host->sin_addr.s_addr)
		return fail("zero-length TCP recvmsg peer");
	if (recv(fd, reply, 0, 0) != 0)
		return fail("zero-length TCP receive");
	errno = 0;
	if (recv(fd, reply, 0, MSG_WAITALL) != -1 || errno != EOPNOTSUPP)
		return fail("zero-length TCP flags");
	errno = 0;
	if (recv(fd, reply, 1, MSG_OOB | MSG_DONTWAIT) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("TCP MSG_OOB receive rejection");
	errno = 0;
	if (send(fd, "x", 1, MSG_OOB | MSG_DONTWAIT) != -1 ||
	    errno != EOPNOTSUPP)
		return fail("TCP MSG_OOB send rejection");
	option.value = 4096;
	errno = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &option.value,
		       sizeof(option.value)) != -1 || errno != ENOPROTOOPT)
		return fail("tcp receive buffer set");
	length = sizeof(option.value);
	errno = 0;
	if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &option.value,
		       &length) != -1 || errno != ENOPROTOOPT)
		return fail("tcp receive buffer get");
	memset(&option, 0xa5, sizeof(option));
	length = sizeof(option);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &option, &length) < 0 ||
	    length != sizeof(option.value) || option.value != SOCK_STREAM)
		return fail("tcp type");
	for (index = sizeof(option.value); index < (int)sizeof(option); index++) {
		if (option.bytes[index] != 0xa5)
			return fail("tcp option overwrite");
	}
	memset(&option, 0xa5, sizeof(option));
	length = 1;
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &option, &length) < 0 ||
	    length != 1 || option.bytes[0] != SOCK_STREAM)
		return fail("tcp truncated option");
	for (index = 1; index < (int)sizeof(option); index++) {
		if (option.bytes[index] != 0xa5)
			return fail("tcp truncated option overwrite");
	}
	option.value = 64;
	if (setsockopt(fd, IPPROTO_IP, IP_TTL, &option.value,
		       sizeof(option.value)) < 0)
		return fail("tcp TTL set");
	length = sizeof(option.value);
	option.value = 0;
	if (getsockopt(fd, IPPROTO_IP, IP_TTL, &option.value, &length) < 0 ||
	    length != sizeof(option.value) || option.value != 64)
		return fail("tcp TTL get");
	for (index = 0; index < 3; index++) {
		option.value = invalid_ttl[index];
		errno = 0;
		if (setsockopt(fd, IPPROTO_IP, IP_TTL, &option.value,
			       sizeof(option.value)) != -1 || errno != EINVAL)
			return fail("tcp invalid TTL");
	}
	length = sizeof(option.value);
	if (getsockopt(fd, IPPROTO_IP, IP_TTL, &option.value, &length) < 0 ||
	    option.value != 64)
		return fail("tcp TTL changed after invalid set");
	option.value = -1;
	if (setsockopt(fd, IPPROTO_IP, IP_TTL, &option.value,
		       sizeof(option.value)) < 0)
		return fail("tcp default TTL set");
	length = sizeof(option.value);
	if (getsockopt(fd, IPPROTO_IP, IP_TTL, &option.value, &length) < 0 ||
	    length != sizeof(option.value) || option.value != DEFAULT_IP_TTL)
		return fail("tcp default TTL get");
	linger.l_linger = MAX_LWIP_LINGER + 1;
	errno = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
		       sizeof(linger)) != -1 || errno != EINVAL)
		return fail("tcp oversized linger");
	linger.l_linger = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
		       sizeof(linger)) < 0)
		return fail("tcp linger set");
	memset(&linger_result, 0, sizeof(linger_result));
	length = sizeof(linger_result);
	if (getsockopt(fd, SOL_SOCKET, SO_LINGER, &linger_result, &length) < 0 ||
	    length != sizeof(linger_result) ||
	    memcmp(&linger, &linger_result, sizeof(linger)))
		return fail("tcp linger get");
	length = sizeof(error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
	    error)
		return fail("tcp error");
	if (write(fd, "tcp-request", 11) != 11)
		return fail("tcp write");
	if (wait_readable(fd) < 0)
		return fail("tcp poll");
	if (ioctl(fd, FIONREAD, &available) < 0 || available != 9)
		return fail("tcp queued bytes before shutdown");
	memset(reply, 0xa5, sizeof(reply));
	fault_iovecs[0].iov_base = reply;
	fault_iovecs[0].iov_len = 4;
	fault_iovecs[1].iov_base = (void *)-1;
	fault_iovecs[1].iov_len = 5;
	errno = 0;
	if (readv(fd, fault_iovecs, 2) != -1 || errno != EFAULT ||
	    ioctl(fd, FIONREAD, &available) < 0 || available != 9 ||
	    (unsigned char)reply[0] != 0xa5)
		return fail("tcp readv fault preserves data");
	if (shutdown(fd, SHUT_RDWR) < 0)
		return fail("tcp shutdown");
	if (ioctl(fd, FIONREAD, &available) < 0 || available != 9)
		return fail("tcp queued bytes after shutdown");
	length = read(fd, reply, 4);
	if (length != 4)
		return fail("tcp partial read after shutdown");
	if (ioctl(fd, FIONREAD, &available) < 0 || available != 5)
		return fail("tcp remaining bytes after shutdown");
	length = recv(fd, reply + 4, sizeof(reply) - 4, 0);
	if (length != 5 || memcmp(reply, "tcp-reply", 9))
		return fail("tcp recv after shutdown");
	if (ioctl(fd, FIONREAD, &available) < 0 || available != 0)
		return fail("tcp drained bytes after shutdown");
	if (recv(fd, reply, sizeof(reply), 0) != 0)
		return fail("tcp EOF after shutdown");
	errno = 0;
	if (send(fd, "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("tcp send after shutdown");
	memset(&message, 0, sizeof(message));
	iovec.iov_base = reply;
	iovec.iov_len = 1;
	message.msg_iov = &iovec;
	message.msg_iovlen = 1;
	errno = 0;
	if (sendmsg(fd, &message, MSG_NOSIGNAL) != -1 || errno != EPIPE)
		return fail("tcp sendmsg after shutdown");
	errno = 0;
	if (write(fd, "x", 1) != -1 || errno != EPIPE)
		return fail("tcp write after shutdown");
	if (close(fd) < 0)
		return fail("tcp close");
	return 0;
}

static int tcp_server_test(const struct sockaddr_in *host)
{
	struct timeval timeout = {
		.tv_sec = 1,
	};
	struct timeval inherited_timeout;
	struct linger linger = {
		.l_onoff = 1,
		.l_linger = 2,
	};
	struct linger inherited_linger;
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(TCP_LISTEN_PORT),
	};
	struct sockaddr_in peer;
	socklen_t length = sizeof(peer);
	char request[12], reply[16];
	int accepted, accepting, fd, flags, nodelay, notify;
	socklen_t option_length;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fail("server socket");
	flags = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags,
		       sizeof(flags)) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		       sizeof(timeout)) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
		       sizeof(timeout)) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
		       sizeof(linger)) < 0 ||
	    bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(fd, 100) < 0)
		return fail("server listen");
	nodelay = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
		       sizeof(nodelay)) < 0)
		return fail("server listener options");
	option_length = sizeof(accepting);
	if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting,
		       &option_length) < 0 || !accepting)
		return fail("server backlog");
	memset(&address, 0, sizeof(address));
	length = sizeof(address);
	if (getsockname(fd, (struct sockaddr *)&address, &length) < 0 ||
	    ntohs(address.sin_port) != TCP_LISTEN_PORT)
		return fail("server getsockname");
	notify = socket(AF_INET, SOCK_DGRAM, 0);
	if (notify < 0 ||
	    sendto(notify, "tcp-listen-ready", 16, 0,
		   (const struct sockaddr *)host, sizeof(*host)) != 16)
		return fail("server notify");
	if (wait_readable(fd) < 0)
		return fail("server poll");
	accepted = accept4(fd, (struct sockaddr *)&peer, &length,
			   SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (accepted < 0)
		return fail("server accept4");
	flags = fcntl(accepted, F_GETFL);
	if (flags < 0 || !(flags & O_NONBLOCK) ||
	    !(fcntl(accepted, F_GETFD) & FD_CLOEXEC))
		return fail("server accept flags");
	memset(&inherited_timeout, 0, sizeof(inherited_timeout));
	option_length = sizeof(inherited_timeout);
	if (getsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
		       &inherited_timeout, &option_length) < 0 ||
	    option_length != sizeof(inherited_timeout) ||
	    memcmp(&inherited_timeout, &timeout, sizeof(timeout)))
		return fail("server inherited receive timeout");
	memset(&inherited_timeout, 0, sizeof(inherited_timeout));
	option_length = sizeof(inherited_timeout);
	if (getsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
		       &inherited_timeout, &option_length) < 0 ||
	    option_length != sizeof(inherited_timeout) ||
	    memcmp(&inherited_timeout, &timeout, sizeof(timeout)))
		return fail("server inherited send timeout");
	memset(&inherited_linger, 0, sizeof(inherited_linger));
	option_length = sizeof(inherited_linger);
	if (getsockopt(accepted, SOL_SOCKET, SO_LINGER,
		       &inherited_linger, &option_length) < 0 ||
	    option_length != sizeof(inherited_linger) ||
	    memcmp(&inherited_linger, &linger, sizeof(linger)))
		return fail("server inherited linger");
	nodelay = 0;
	option_length = sizeof(nodelay);
	if (getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &nodelay,
		       &option_length) < 0 || option_length != sizeof(nodelay) ||
	    !nodelay)
		return fail("server inherited TCP_NODELAY");
	if (wait_readable(accepted) < 0 ||
	    read_all(accepted, request, 12) < 0 ||
	    memcmp(request, "host-request", 12) ||
	    write_all(accepted, "guest-reply", 11) < 0)
		return fail("server exchange");
	if (wait_readable(notify) < 0 ||
	    recv(notify, reply, sizeof(reply), 0) != 9 ||
	    memcmp(reply, "server-ok", 9))
		return fail("server ack");
	if (close(accepted) < 0 || close(notify) < 0 || close(fd) < 0)
		return fail("server close");
	return 0;
}

static int tcp_linger_reclaim_test(struct sockaddr_in *host)
{
	struct linger linger = {
		.l_onoff = 1,
		.l_linger = 1,
	};
	int attempt, fd, flags;

	host->sin_port = htons(TCP_LINGER_PORT);
	for (attempt = 0; attempt < 20; attempt++) {
		linger.l_linger = attempt ? 0 : 1;
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0 ||
		    connect(fd, (const struct sockaddr *)host,
			    sizeof(*host)) < 0)
			return fail("linger connect");
		flags = fcntl(fd, F_GETFL);
		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
		    setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
			       sizeof(linger)) < 0)
			return fail("linger setup");
		for (;;) {
			ssize_t written = send(fd, bulk_send,
					       sizeof(bulk_send), MSG_NOSIGNAL);

			if (written > 0)
				continue;
			if (written < 0 && errno == EAGAIN)
				break;
			return fail("linger fill");
		}
		if (close(fd) < 0)
			return fail("linger close");
	}
	return 0;
}

static int tcp_bulk_test(struct sockaddr_in *host)
{
	uint32_t length = htonl(TCP_BULK_SIZE);
	int fd, index;

	for (index = 0; index < TCP_BULK_SIZE; index++)
		bulk_send[index] = index * 37U + 11U;
	host->sin_port = htons(TCP_BULK_PORT);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 ||
	    connect(fd, (const struct sockaddr *)host, sizeof(*host)) < 0)
		return fail("bulk connect");
	if (write_all(fd, &length, sizeof(length)) < 0 ||
	    write_all(fd, bulk_send, sizeof(bulk_send)) < 0 ||
	    read_all(fd, bulk_receive, sizeof(bulk_receive)) < 0 ||
	    memcmp(bulk_send, bulk_receive, sizeof(bulk_send)))
		return fail("bulk exchange");
	if (close(fd) < 0)
		return fail("bulk close");
	return 0;
}

static int poll_timeout_test(void)
{
	struct pollfd pollfd = {
		.fd = -1,
		.events = POLLIN,
	};

	if (poll(&pollfd, 1, 20) != 0 || pollfd.revents)
		return fail("poll timeout");
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_in host = {
		.sin_family = AF_INET,
		.sin_port = htons(UDP_PORT),
	};
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) < 0 || time.tv_sec < 0)
		return fail("clock_gettime");
	if (sigpipe_test() || protocol_test() || broadcast_permission_test() ||
	    loopback_test() || inherited_poll_test())
		return 1;
	if (argc == 2 && !strcmp(argv[1], "loopback")) {
		puts("NETWORK_LOOPBACK_OK");
		return 0;
	}
	if (inet_pton(AF_INET, FIXTURE_ADDRESS, &host.sin_addr) != 1)
		return fail("inet_pton");
	if (icmp_test(&host))
		return 1;
	if (udp_test(&host))
		return 1;
	host.sin_port = htons(TCP_PORT);
	if (tcp_test(&host))
		return 1;
	if (tcp_linger_reclaim_test(&host))
		return 1;
	host.sin_port = htons(UDP_PORT);
	if (tcp_server_test(&host) || tcp_bulk_test(&host) ||
	    poll_timeout_test())
		return 1;
	puts("NETWORK_RUNTIME_OK");
	return 0;
}
