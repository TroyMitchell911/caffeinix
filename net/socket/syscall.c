#include <file.h>
#include <ksocket.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <vfs.h>
#include <vm.h>

#define SOCKET_OPTION_MAX 32

static int socket_address_in(uint64 user_address, uint32 length,
			     struct linux_sockaddr_in *address)
{
	if (!user_address)
		return -LINUX_EFAULT;
	if (length < sizeof(*address))
		return -LINUX_EINVAL;
	if (copyin(cur_proc()->pagetable, (char *)address, user_address,
		   sizeof(*address)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

static int socket_address_out(uint64 user_address,
			      uint64 user_length_address,
			      const struct linux_sockaddr_in *address)
{
	process_t process = cur_proc();
	uint32 length, copied;

	if (!user_address)
		return 0;
	if (!user_length_address ||
	    copyin(process->pagetable, (char *)&length,
		   user_length_address, sizeof(length)) < 0)
		return -LINUX_EFAULT;
	copied = length < sizeof(*address) ? length : sizeof(*address);
	if (copied && copyout(process->pagetable, user_address,
			      (char *)address, copied) < 0)
		return -LINUX_EFAULT;
	length = sizeof(*address);
	if (copyout(process->pagetable, user_length_address,
		    (char *)&length, sizeof(length)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_socket(void)
{
	int family, type, protocol, fd, result;

	argint(0, &family);
	argint(1, &type);
	argint(2, &protocol);
	result = ksocket_create(family, type, protocol, &fd);
	return result < 0 ? result : fd;
}

uint64 sys_linux_socketpair(void)
{
	int family;

	argint(0, &family);
	return family == LINUX_AF_INET ?
		-LINUX_EOPNOTSUPP : -LINUX_EAFNOSUPPORT;
}

uint64 sys_linux_bind(void)
{
	struct linux_sockaddr_in address;
	uint64 user_address;
	int fd, length, result;

	argint(0, &fd);
	argaddr(1, &user_address);
	argint(2, &length);
	result = socket_address_in(user_address, length, &address);
	return result < 0 ? result : ksocket_bind(fd, &address);
}

uint64 sys_linux_listen(void)
{
	int fd, backlog;

	argint(0, &fd);
	argint(1, &backlog);
	return ksocket_listen(fd, backlog);
}

static uint64 socket_accept(int flags)
{
	struct linux_sockaddr_in address;
	uint64 user_address, user_length_address;
	int fd, newfd, result;

	argint(0, &fd);
	argaddr(1, &user_address);
	argaddr(2, &user_length_address);
	if (user_address && !user_length_address)
		return -LINUX_EFAULT;
	result = ksocket_accept(fd, user_address ? &address : 0,
				flags, &newfd);
	if (result < 0)
		return result;
	result = socket_address_out(user_address, user_length_address,
				    &address);
	if (result < 0) {
		vfs_close(newfd);
		return result;
	}
	return newfd;
}

uint64 sys_linux_accept(void)
{
	return socket_accept(0);
}

uint64 sys_linux_accept4(void)
{
	int flags;

	argint(3, &flags);
	return socket_accept(flags);
}

uint64 sys_linux_connect(void)
{
	struct linux_sockaddr_in address;
	uint64 user_address;
	int fd, length, result;

	argint(0, &fd);
	argaddr(1, &user_address);
	argint(2, &length);
	result = socket_address_in(user_address, length, &address);
	return result < 0 ? result : ksocket_connect(fd, &address);
}

static uint64 socket_get_name(int peer)
{
	struct linux_sockaddr_in address;
	uint64 user_address, user_length_address;
	int fd, result;

	argint(0, &fd);
	argaddr(1, &user_address);
	argaddr(2, &user_length_address);
	if (!user_address || !user_length_address)
		return -LINUX_EFAULT;
	result = ksocket_get_name(fd, peer, &address);
	if (result < 0)
		return result;
	return socket_address_out(user_address, user_length_address,
				  &address);
}

uint64 sys_linux_getsockname(void)
{
	return socket_get_name(0);
}

uint64 sys_linux_getpeername(void)
{
	return socket_get_name(1);
}

static int socket_send_length(int fd, uint64 requested, uint64 *length)
{
	int type, result;

	if (requested <= PGSIZE) {
		*length = requested;
		return 0;
	}
	result = ksocket_type(fd, &type);
	if (result < 0)
		return result;
	if (type != LINUX_SOCK_STREAM)
		return -LINUX_EMSGSIZE;
	*length = PGSIZE;
	return 0;
}

static uint64 socket_receive_length(uint64 requested)
{
	return requested > PGSIZE ? PGSIZE : requested;
}

uint64 sys_linux_sendto(void)
{
	struct linux_sockaddr_in address;
	process_t process = cur_proc();
	uint64 user_buffer, requested, length, user_address;
	void *buffer;
	int fd, flags, address_length, result;
	int64 sent;

	argint(0, &fd);
	argaddr(1, &user_buffer);
	argaddr(2, &requested);
	argint(3, &flags);
	argaddr(4, &user_address);
	argint(5, &address_length);
	result = socket_send_length(fd, requested, &length);
	if (result < 0)
		return result;
	if (user_address) {
		result = socket_address_in(user_address, address_length,
					   &address);
		if (result < 0)
			return result;
	}
	if (!length)
		return ksocket_send(fd, "", 0, flags,
				    user_address ? &address : 0);
	buffer = palloc();
	if (!buffer)
		return -LINUX_ENOMEM;
	if (copyin(process->pagetable, buffer, user_buffer, length) < 0) {
		pfree(buffer);
		return -LINUX_EFAULT;
	}
	sent = ksocket_send(fd, buffer, length, flags,
			    user_address ? &address : 0);
	pfree(buffer);
	return sent;
}

uint64 sys_linux_recvfrom(void)
{
	struct linux_sockaddr_in address;
	process_t process = cur_proc();
	uint64 user_buffer, requested, length;
	uint64 user_address, user_length_address;
	uint64 copied;
	void *buffer;
	int fd, flags, result;
	int64 received;

	argint(0, &fd);
	argaddr(1, &user_buffer);
	argaddr(2, &requested);
	argint(3, &flags);
	argaddr(4, &user_address);
	argaddr(5, &user_length_address);
	if (user_address && !user_length_address)
		return -LINUX_EFAULT;
	length = socket_receive_length(requested);
	buffer = length ? palloc() : 0;
	if (length && !buffer)
		return -LINUX_ENOMEM;
	received = ksocket_receive(fd, buffer, length, flags,
				   user_address ? &address : 0);
	copied = received > 0 ? (uint64)received : 0;
	if (copied > length)
		copied = length;
	if (copied &&
	    copyout(process->pagetable, user_buffer, buffer, copied) < 0)
		received = -LINUX_EFAULT;
	if (received >= 0 && user_address) {
		result = socket_address_out(user_address,
			user_length_address, &address);
		if (result < 0)
			received = result;
	}
	if (buffer)
		pfree(buffer);
	return received;
}

static uint32 socket_option_size(int level, int option)
{
	if (level == LINUX_SOL_SOCKET) {
		switch (option) {
		case LINUX_SO_LINGER:
			return sizeof(struct linux_linger);
		case LINUX_SO_RCVTIMEO:
		case LINUX_SO_SNDTIMEO:
			return sizeof(struct linux_timeval);
		case LINUX_SO_REUSEADDR:
		case LINUX_SO_TYPE:
		case LINUX_SO_ERROR:
		case LINUX_SO_BROADCAST:
		case LINUX_SO_RCVBUF:
		case LINUX_SO_KEEPALIVE:
		case LINUX_SO_ACCEPTCONN:
			return sizeof(int32);
		}
	}
	if ((level == LINUX_IPPROTO_IP && option == LINUX_IP_TTL) ||
	    (level == LINUX_IPPROTO_TCP && option == LINUX_TCP_NODELAY))
		return sizeof(int32);
	return 0;
}

uint64 sys_linux_setsockopt(void)
{
	uint8 value[SOCKET_OPTION_MAX];
	process_t process = cur_proc();
	uint64 user_value;
	int fd, level, option, length;

	argint(0, &fd);
	argint(1, &level);
	argint(2, &option);
	argaddr(3, &user_value);
	argint(4, &length);
	if (length < 0 || length > SOCKET_OPTION_MAX)
		return -LINUX_EINVAL;
	if (length && copyin(process->pagetable, (char *)value,
			     user_value, length) < 0)
		return -LINUX_EFAULT;
	return ksocket_set_option(fd, level, option, value, length);
}

uint64 sys_linux_getsockopt(void)
{
	uint8 value[SOCKET_OPTION_MAX];
	process_t process = cur_proc();
	uint64 user_value, user_length;
	uint32 expected, length;
	int fd, level, option, result;

	argint(0, &fd);
	argint(1, &level);
	argint(2, &option);
	argaddr(3, &user_value);
	argaddr(4, &user_length);
	if (!user_length ||
	    copyin(process->pagetable, (char *)&length, user_length,
		   sizeof(length)) < 0)
		return -LINUX_EFAULT;
	if (length > SOCKET_OPTION_MAX)
		length = SOCKET_OPTION_MAX;
	memset(value, 0, sizeof(value));
	result = ksocket_get_option(fd, level, option, value, &length);
	if (result < 0)
		return result;
	expected = socket_option_size(level, option);
	if (expected && length > expected)
		length = expected;
	if ((length && copyout(process->pagetable, user_value,
			       (char *)value, length) < 0) ||
	    copyout(process->pagetable, user_length, (char *)&length,
		    sizeof(length)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_shutdown(void)
{
	int fd, how;

	argint(0, &fd);
	argint(1, &how);
	return ksocket_shutdown(fd, how);
}

static int socket_copy_message(struct linux_msghdr *message,
			       struct linux_iovec *iovecs,
			       int receive)
{
	process_t process = cur_proc();
	uint64 message_address;

	argaddr(1, &message_address);
	if (copyin(process->pagetable, (char *)message, message_address,
		   sizeof(*message)) < 0)
		return -LINUX_EFAULT;
	if (message->iov_length > LINUX_IOV_MAX)
		return -LINUX_EINVAL;
	if (message->iov_length &&
	    copyin(process->pagetable, (char *)iovecs, message->iov,
		   message->iov_length * sizeof(*iovecs)) < 0)
		return -LINUX_EFAULT;
	if (!receive && message->control_length)
		return -LINUX_EOPNOTSUPP;
	return 0;
}

uint64 sys_linux_sendmsg(void)
{
	struct linux_sockaddr_in address;
	struct linux_iovec iovecs[LINUX_IOV_MAX];
	struct linux_msghdr message;
	process_t process = cur_proc();
	uint64 used = 0;
	void *buffer = 0;
	int fd, flags, type, result;
	uint32 i;

	argint(0, &fd);
	argint(2, &flags);
	result = socket_copy_message(&message, iovecs, 0);
	if (result < 0)
		return result;
	result = ksocket_type(fd, &type);
	if (result < 0)
		return result;
	if (message.name) {
		result = socket_address_in(message.name, message.name_length,
					   &address);
		if (result < 0)
			return result;
	}
	for (i = 0; i < message.iov_length; i++) {
		uint64 copied = iovecs[i].len;

		if (copied > PGSIZE - used) {
			if (type != LINUX_SOCK_STREAM) {
				if (buffer)
					pfree(buffer);
				return -LINUX_EMSGSIZE;
			}
			copied = PGSIZE - used;
		}
		if (copied && !buffer) {
			buffer = palloc();
			if (!buffer)
				return -LINUX_ENOMEM;
		}
		if (copied && copyin(process->pagetable, buffer + used,
				     iovecs[i].base, copied) < 0) {
			pfree(buffer);
			return -LINUX_EFAULT;
		}
		used += copied;
		if (copied != iovecs[i].len)
			break;
	}
	result = ksocket_send(fd, buffer ? buffer : "", used, flags,
			      message.name ? &address : 0);
	if (buffer)
		pfree(buffer);
	return result;
}

uint64 sys_linux_recvmsg(void)
{
	struct linux_sockaddr_in address;
	struct linux_iovec iovecs[LINUX_IOV_MAX];
	struct linux_msghdr message;
	process_t process = cur_proc();
	uint64 message_address, capacity = 0, copied = 0, payload_length;
	void *buffer;
	uint32 message_flags;
	int fd, flags, result;
	int64 received;
	uint32 i;

	argint(0, &fd);
	argaddr(1, &message_address);
	argint(2, &flags);
	result = socket_copy_message(&message, iovecs, 1);
	if (result < 0)
		return result;
	for (i = 0; i < message.iov_length && capacity < PGSIZE; i++) {
		if (iovecs[i].len > PGSIZE - capacity)
			capacity = PGSIZE;
		else
			capacity += iovecs[i].len;
	}
	buffer = capacity ? palloc() : 0;
	if (capacity && !buffer)
		return -LINUX_ENOMEM;
	received = ksocket_receive_message(fd, buffer, capacity, flags,
		message.name ? &address : 0, &message_flags);
	if (received < 0) {
		if (buffer)
			pfree(buffer);
		return received;
	}
	payload_length = (uint64)received < capacity ? received : capacity;
	for (i = 0; i < message.iov_length && copied < payload_length;
	     i++) {
		uint64 part = iovecs[i].len;

		if (part > payload_length - copied)
			part = payload_length - copied;
		if (part && copyout(process->pagetable, iovecs[i].base,
				    buffer + copied, part) < 0) {
			if (buffer)
				pfree(buffer);
			return -LINUX_EFAULT;
		}
		copied += part;
	}
	if (buffer)
		pfree(buffer);
	message.flags = message_flags;
	message.control_length = 0;
	if (message.name) {
		uint32 available = message.name_length;
		uint32 part = available < sizeof(address) ?
			available : sizeof(address);

		if (part && copyout(process->pagetable, message.name,
				    (char *)&address, part) < 0)
			return -LINUX_EFAULT;
		message.name_length = sizeof(address);
	}
	if (copyout(process->pagetable, message_address, (char *)&message,
		    sizeof(message)) < 0)
		return -LINUX_EFAULT;
	return received;
}

uint64 sys_linux_ppoll(void)
{
	struct linux_timespec time;
	struct linux_pollfd fds[NOFILE];
	struct vfs_pollfd pollfds[NOFILE];
	process_t process = cur_proc();
	uint64 fds_address, timeout_address, mask_address;
	uint64 milliseconds;
	int count, index, timeout = -1, result;

	argaddr(0, &fds_address);
	argint(1, &count);
	argaddr(2, &timeout_address);
	argaddr(3, &mask_address);
	if (count < 0 || count > NOFILE)
		return -LINUX_EINVAL;
	if (mask_address)
		return -LINUX_EOPNOTSUPP;
	if (timeout_address) {
		if (copyin(process->pagetable, (char *)&time,
			   timeout_address, sizeof(time)) < 0)
			return -LINUX_EFAULT;
		if (time.seconds < 0 || time.nanoseconds < 0 ||
		    time.nanoseconds >= 1000000000)
			return -LINUX_EINVAL;
		if ((uint64)time.seconds > 0x7fffffffULL / 1000)
			milliseconds = 0x7fffffff;
		else
			milliseconds = time.seconds * 1000ULL +
				       (time.nanoseconds + 999999) / 1000000;
		timeout = milliseconds > 0x7fffffff ?
			0x7fffffff : milliseconds;
	}
	if (count && copyin(process->pagetable, (char *)fds, fds_address,
			    count * sizeof(*fds)) < 0)
		return -LINUX_EFAULT;
	for (index = 0; index < count; index++) {
		pollfds[index].fd = fds[index].fd;
		pollfds[index].events = 0;
		if (fds[index].events & LINUX_POLLIN)
			pollfds[index].events |= VFS_POLL_IN;
		if (fds[index].events & LINUX_POLLOUT)
			pollfds[index].events |= VFS_POLL_OUT;
	}
	result = vfs_poll(pollfds, count, timeout);
	if (result < 0)
		return -LINUX_EINVAL;
	for (index = 0; index < count; index++) {
		fds[index].revents = 0;
		if (pollfds[index].revents & VFS_POLL_IN)
			fds[index].revents |= LINUX_POLLIN;
		if (pollfds[index].revents & VFS_POLL_OUT)
			fds[index].revents |= LINUX_POLLOUT;
		if (pollfds[index].revents & VFS_POLL_ERR)
			fds[index].revents |= LINUX_POLLERR;
		if (pollfds[index].revents & VFS_POLL_HUP)
			fds[index].revents |= LINUX_POLLHUP;
		if (pollfds[index].revents & VFS_POLL_NVAL)
			fds[index].revents |= LINUX_POLLNVAL;
	}
	if (count && copyout(process->pagetable, fds_address, (char *)fds,
			     count * sizeof(*fds)) < 0)
		return -LINUX_EFAULT;
	return result;
}
