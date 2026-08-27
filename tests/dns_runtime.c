#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define DNS_PORT 53
#define DNS_HEADER_SIZE 12
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1

static uint16_t read_u16(const unsigned char *value)
{
	return (uint16_t)value[0] << 8 | value[1];
}

static void write_u16(unsigned char *value, uint16_t number)
{
	value[0] = number >> 8;
	value[1] = number;
}

static size_t question_end(const unsigned char *packet, size_t length)
{
	size_t offset = DNS_HEADER_SIZE;

	while (offset < length && packet[offset]) {
		if (packet[offset] > 63 || packet[offset] + offset + 1 >= length)
			return 0;
		offset += packet[offset] + 1;
	}
	if (offset + 5 > length)
		return 0;
	return offset + 5;
}

static int answer_query(int socket_fd, unsigned char *packet, size_t length,
			struct sockaddr_in *peer, socklen_t peer_length)
{
	static const unsigned char answer[] = {
		0xc0, 0x0c, 0x00, DNS_TYPE_A, 0x00, DNS_CLASS_IN,
		0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 10, 0, 2, 2,
	};
	size_t end;
	uint16_t query_class, query_type;
	int has_answer;

	if (length < DNS_HEADER_SIZE || read_u16(packet + 4) != 1)
		return 0;
	end = question_end(packet, length);
	if (!end)
		return 0;
	query_type = read_u16(packet + end - 4);
	query_class = read_u16(packet + end - 2);
	has_answer = query_type == DNS_TYPE_A && query_class == DNS_CLASS_IN;
	packet[2] = 0x81;
	packet[3] = 0x80;
	write_u16(packet + 6, has_answer ? 1 : 0);
	write_u16(packet + 8, 0);
	write_u16(packet + 10, 0);
	length = end;
	if (has_answer) {
		if (length + sizeof(answer) > 512)
			return 0;
		memcpy(packet + length, answer, sizeof(answer));
		length += sizeof(answer);
	}
	if (sendto(socket_fd, packet, length, 0,
		   (struct sockaddr *)peer, peer_length) != (ssize_t)length)
		return 0;
	return has_answer;
}

static int serve(int ready_fd, int announce)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(DNS_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct pollfd poll_fd;
	int socket_fd, served = 0;

	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0 || bind(socket_fd, (struct sockaddr *)&address,
				 sizeof(address)) < 0)
		return 1;
	if (ready_fd >= 0 && write(ready_fd, "R", 1) != 1)
		return 1;
	if (announce) {
		puts("DNS_FIXTURE_READY");
		fflush(stdout);
	}
	poll_fd.fd = socket_fd;
	poll_fd.events = POLLIN;
	while (poll(&poll_fd, 1, served ? 100 : 10000) > 0) {
		struct sockaddr_in peer;
		unsigned char packet[512];
		socklen_t peer_length = sizeof(peer);
		ssize_t length;

		length = recvfrom(socket_fd, packet, sizeof(packet), 0,
				  (struct sockaddr *)&peer, &peer_length);
		if (length > 0 &&
		    answer_query(socket_fd, packet, length, &peer, peer_length))
			served = 1;
	}
	close(socket_fd);
	return served ? 0 : 1;
}

static int resolve_fixture(void)
{
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct sockaddr_in *address;
	struct addrinfo *result;
	unsigned char expected[4] = { 10, 0, 2, 2 };
	char ready;
	int matched = 0, pipe_fd[2], status;
	pid_t child;

	if (pipe(pipe_fd) < 0)
		return 1;
	child = fork();
	if (child < 0) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return 1;
	}
	if (!child) {
		close(pipe_fd[0]);
		_exit(serve(pipe_fd[1], 0));
	}
	close(pipe_fd[1]);
	if (read(pipe_fd[0], &ready, 1) == 1 && ready == 'R' &&
	    !getaddrinfo("fixture.test", "80", &hints, &result)) {
		address = (struct sockaddr_in *)result->ai_addr;
		matched = result->ai_addrlen == sizeof(*address) &&
			  !memcmp(&address->sin_addr, expected,
				  sizeof(expected));
		freeaddrinfo(result);
	}
	close(pipe_fd[0]);
	if (!matched)
		kill(child, SIGTERM);
	if (waitpid(child, &status, 0) < 0 ||
	    !WIFEXITED(status) || WEXITSTATUS(status))
		return 1;
	return matched ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "serve"))
		return serve(-1, 1);
	if (argc != 1 || resolve_fixture()) {
		printf("DNS_RUNTIME_FAIL errno=%d\n", errno);
		return EXIT_FAILURE;
	}
	puts("DNS_RUNTIME_OK");
	return EXIT_SUCCESS;
}
