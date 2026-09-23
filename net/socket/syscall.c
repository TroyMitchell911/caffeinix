/*
 * Linux RISC-V socket syscall argument marshaling.
 *
 * This file copies UAPI objects to temporary kernel memory and delegates all
 * protocol behavior to ksocket.  User pointers are never retained past a call.
 */
#include <file.h>
#include <ksocket.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <signal.h>
#include <syscall.h>
#include <vfs.h>
#include <vm.h>

#define SOCKET_OPTION_MAX 32

/* Validate and copy one IPv4 sockaddr from the current process. */
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

/*
 * Honor Linux's in/out sockaddr length convention while copying to userspace.
 */
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

/**
 * sys_linux_socket() - Install a socket described by syscall arguments
 *
 * Decodes family, type/creation flags, and protocol from the current frame.
 *
 * Context: User syscall context; may sleep while creating the backend.
 * Return: New descriptor or a negative Linux errno.
 */
uint64 sys_linux_socket(void)
{
	int family, type, protocol, fd, result;

	argint(0, &family);
	argint(1, &type);
	argint(2, &protocol);
	result = ksocket_create(family, type, protocol, &fd);
	return result < 0 ? result : fd;
}

/**
 * sys_linux_socketpair() - Reject the unsupported paired-socket operation
 *
 * Context: User syscall context; reads only the family argument.
 * Return: -EOPNOTSUPP for AF_INET, otherwise -EAFNOSUPPORT.
 */
uint64 sys_linux_socketpair(void)
{
	int family;

	argint(0, &family);
	return family == LINUX_AF_INET ?
		-LINUX_EOPNOTSUPP : -LINUX_EAFNOSUPPORT;
}

/**
 * sys_linux_bind() - Copy a user IPv4 sockaddr and bind its socket
 *
 * Decodes descriptor, address, and address length; never retains user memory.
 *
 * Context: User syscall context; user copies and stack calls may sleep.
 * Return: Zero or a negative Linux errno.
 */
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

/**
 * sys_linux_listen() - Listen on the descriptor from the syscall frame
 *
 * Context: User syscall context; backend operations may sleep.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_listen(void)
{
	int fd, backlog;

	argint(0, &fd);
	argint(1, &backlog);
	return ksocket_listen(fd, backlog);
}

/* Shared accept and accept4 marshaling, including close-on-copyout failure. */
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

/**
 * sys_linux_accept() - Accept a connection without creation flags
 *
 * A failed peer-address copy closes the newly installed child descriptor.
 *
 * Context: User syscall context; may sleep unless the listener is
 *          nonblocking.
 * Return: Child descriptor or a negative Linux errno.
 */
uint64 sys_linux_accept(void)
{
	return socket_accept(0);
}

/**
 * sys_linux_accept4() - Accept a connection with child-descriptor flags
 *
 * The fourth register argument supplies flags; these affect the new socket,
 * not whether waiting on the listening socket blocks.
 *
 * Context: User syscall context; may sleep while waiting or copying
 *          addresses.
 * Return: Child descriptor or a negative Linux errno.
 */
uint64 sys_linux_accept4(void)
{
	int flags;

	argint(3, &flags);
	return socket_accept(flags);
}

/**
 * sys_linux_connect() - Stage an IPv4 connection request from user memory
 *
 * Context: User syscall context; copying or a blocking connect may sleep.
 * Return: Zero or a negative Linux errno, including nonblocking progress.
 */
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

/* Shared getsockname and getpeername marshaling. */
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

/**
 * sys_linux_getsockname() - Export the local address and its actual length
 *
 * Context: User syscall context; stack queries and user copies may sleep.
 * Return: Zero or a negative Linux errno; a failed copy may modify a prefix.
 */
uint64 sys_linux_getsockname(void)
{
	return socket_get_name(0);
}

/**
 * sys_linux_getpeername() - Export the peer address and its actual length
 *
 * Context: User syscall context; stack queries and user copies may sleep.
 * Return: Zero or a negative Linux errno; a failed copy may modify a prefix.
 */
uint64 sys_linux_getpeername(void)
{
	return socket_get_name(1);
}

/* Bound one syscall copy to a page; reject oversized datagrams. */
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

/* Bound receive staging memory to a page. */
static uint64 socket_receive_length(uint64 requested)
{
	return requested > PGSIZE ? PGSIZE : requested;
}

/**
 * sys_linux_sendto() - Transmit one page-bounded copy of a user payload
 *
 * Streams may return a short count; oversized datagrams are rejected rather
 * than split. An optional destination is copied before the send operation.
 *
 * Context: User syscall context; may allocate, fault, or wait in the stack.
 * Return: Bytes accepted or a negative Linux errno.
 */
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

/**
 * sys_linux_recvfrom() - Receive through a page-sized kernel staging buffer
 *
 * Copies no more than the staging capacity even when MSG_TRUNC reports a
 * larger datagram. A later copy fault cannot undo data consumed by the stack.
 *
 * Context: User syscall context; may allocate, fault, or wait for data.
 * Return: Backend receive count or a negative Linux errno.
 */
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

/* Return the exact Linux ABI storage size for a supported socket option. */
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

/**
 * sys_linux_setsockopt() - Stage a bounded user socket-option value
 *
 * Rejects values larger than SOCKET_OPTION_MAX before copying;
 * option-specific validation belongs to the socket backend.
 *
 * Context: User syscall context; user copies and backend operations may
 *          sleep.
 * Return: Zero or a negative Linux errno.
 */
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

/**
 * sys_linux_getsockopt() - Export a socket option and its copied length
 *
 * Caps the staging buffer and supported option layout before copying out. A
 * copy fault may occur after the backend query or a partial user write.
 *
 * Context: User syscall context; user copies and backend queries may sleep.
 * Return: Zero or a negative Linux errno.
 */
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

/**
 * sys_linux_shutdown() - Apply the syscall frame's directional shutdown
 *
 * Context: User syscall context; backend operations may sleep.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_shutdown(void)
{
	int fd, how;

	argint(0, &fd);
	argint(1, &how);
	return ksocket_shutdown(fd, how);
}

/* Copy msghdr/iovec metadata and reject unsupported ancillary send data. */
static int socket_copy_message(struct linux_msghdr *message,
			       struct vfs_iovec **iovecs,
			       unsigned int *iov_order,
			       int receive)
{
	process_t process = cur_proc();
	uint64 message_address;
	int result;

	argaddr(1, &message_address);
	if (copyin(process->pagetable, (char *)message, message_address,
		   sizeof(*message)) < 0)
		return -LINUX_EFAULT;
	if (message->iov_length > LINUX_IOV_MAX)
		return -LINUX_EINVAL;
	result = copy_user_iov(message->iov, message->iov_length, iovecs,
	                       iov_order);
	if (result < 0)
		return result;
	if (!receive && message->control_length)
		result = -LINUX_EOPNOTSUPP;
	if (result < 0 && *iovecs) {
		free_pages(*iovecs, *iov_order);
		*iovecs = 0;
	}
	return result;
}

/* Release the page allocation returned by copy_user_iov(). */
static void socket_free_iovecs(struct vfs_iovec *iovecs,
			       unsigned int order)
{
	if (iovecs)
		free_pages(iovecs, order);
}

/**
 * sys_linux_sendmsg() - Gather one page-bounded message from user iovecs
 *
 * Rejects ancillary send data and oversized datagrams. Stream iovecs may be
 * consumed only through the page-sized prefix; no user pointer is retained.
 *
 * Context: User syscall context; allocation, copies, and stack calls may
 *          sleep.
 * Return: Bytes accepted or a negative Linux errno.
 */
uint64 sys_linux_sendmsg(void)
{
	struct linux_sockaddr_in address;
	struct vfs_iovec *iovecs;
	struct linux_msghdr message;
	process_t process = cur_proc();
	uint64 used = 0;
	unsigned int iov_order;
	void *buffer = 0;
	int fd, flags, type, result;
	uint32 i;

	argint(0, &fd);
	argint(2, &flags);
	result = socket_copy_message(&message, &iovecs, &iov_order, 0);
	if (result < 0)
		return result;
	result = ksocket_type(fd, &type);
	if (result < 0)
		goto out_iov;
	if (message.name) {
		result = socket_address_in(message.name, message.name_length,
					   &address);
		if (result < 0)
			goto out_iov;
	}
	for (i = 0; i < message.iov_length; i++) {
		uint64 copied = iovecs[i].length;

		if (copied > PGSIZE - used) {
			if (type != LINUX_SOCK_STREAM) {
				result = -LINUX_EMSGSIZE;
				goto out_buffer;
			}
			copied = PGSIZE - used;
		}
		if (copied && !buffer) {
			buffer = palloc();
			if (!buffer) {
				result = -LINUX_ENOMEM;
				goto out_iov;
			}
		}
		if (copied && copyin(process->pagetable, buffer + used,
				     iovecs[i].base, copied) < 0) {
			result = -LINUX_EFAULT;
			goto out_buffer;
		}
		used += copied;
		if (copied != iovecs[i].length)
			break;
	}
	result = ksocket_send(fd, buffer ? buffer : "", used, flags,
			      message.name ? &address : 0);
out_buffer:
	if (buffer)
		pfree(buffer);
out_iov:
	socket_free_iovecs(iovecs, iov_order);
	return result;
}

/**
 * sys_linux_recvmsg() - Scatter a received message into bounded user iovecs
 *
 * Exports address length and message flags, but no ancillary data. Copyout
 * errors can occur after data is consumed and after earlier iovecs were
 * filled.
 *
 * Context: User syscall context; allocation, copies, and stack calls may
 *          sleep.
 * Return: Backend receive count, possibly MSG_TRUNC length, or negative
 *         errno.
 */
uint64 sys_linux_recvmsg(void)
{
	struct linux_sockaddr_in address;
	struct vfs_iovec *iovecs;
	struct linux_msghdr message;
	process_t process = cur_proc();
	uint64 message_address, capacity = 0, copied = 0, payload_length;
	unsigned int iov_order;
	void *buffer;
	uint32 message_flags;
	int fd, flags, result;
	int64 received;
	uint32 i;

	argint(0, &fd);
	argaddr(1, &message_address);
	argint(2, &flags);
	result = socket_copy_message(&message, &iovecs, &iov_order, 1);
	if (result < 0)
		return result;
	for (i = 0; i < message.iov_length && capacity < PGSIZE; i++) {
		if (iovecs[i].length > PGSIZE - capacity)
			capacity = PGSIZE;
		else
			capacity += iovecs[i].length;
	}
	buffer = capacity ? palloc() : 0;
	if (capacity && !buffer) {
		result = -LINUX_ENOMEM;
		goto out_iov;
	}
	received = ksocket_receive_message(fd, buffer, capacity, flags,
		message.name ? &address : 0, &message_flags);
	if (received < 0) {
		result = received;
		goto out_buffer;
	}
	payload_length = (uint64)received < capacity ? received : capacity;
	for (i = 0; i < message.iov_length && copied < payload_length;
	     i++) {
		uint64 part = iovecs[i].length;

		if (part > payload_length - copied)
			part = payload_length - copied;
		if (part && copyout(process->pagetable, iovecs[i].base,
				    buffer + copied, part) < 0) {
			result = -LINUX_EFAULT;
			goto out_buffer;
		}
		copied += part;
	}
	message.flags = message_flags;
	message.control_length = 0;
	if (message.name) {
		uint32 available = message.name_length;
		uint32 part = available < sizeof(address) ?
			available : sizeof(address);

		if (part && copyout(process->pagetable, message.name,
				    (char *)&address, part) < 0) {
			result = -LINUX_EFAULT;
			goto out_buffer;
		}
		message.name_length = sizeof(address);
	}
	if (copyout(process->pagetable, message_address, (char *)&message,
		    sizeof(message)) < 0) {
		result = -LINUX_EFAULT;
		goto out_buffer;
	}
	result = received;
out_buffer:
	if (buffer)
		pfree(buffer);
out_iov:
	socket_free_iovecs(iovecs, iov_order);
	return result;
}

/**
 * sys_linux_ppoll() - Poll descriptors with a temporary signal mask
 *
 * Limits the array to NOFILE entries and rounds timeout nanoseconds upward to
 * milliseconds. On interruption the user-return signal path restores the
 * saved mask; otherwise this call restores it before copying readiness back.
 *
 * Context: User syscall context; may fault and sleep on the global poll
 *          queue.
 * Return: Ready entry count, zero on timeout, or a negative Linux errno.
 */
uint64 sys_linux_ppoll(void)
{
	struct linux_timespec time;
	struct linux_pollfd fds[NOFILE];
	struct vfs_pollfd pollfds[NOFILE];
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 fds_address, timeout_address, mask_address, mask_size;
	uint64 milliseconds, mask = 0, old_mask = 0;
	int count, index, timeout = -1, result;

	argaddr(0, &fds_address);
	argint(1, &count);
	argaddr(2, &timeout_address);
	argaddr(3, &mask_address);
	argaddr(4, &mask_size);
	if (count < 0 || count > NOFILE)
		return -LINUX_EINVAL;
	if (mask_address && mask_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (mask_address &&
	    copyin(process->pagetable, (char *)&mask, mask_address,
	           sizeof(mask)) < 0)
		return -LINUX_EFAULT;
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
	if (mask_address) {
		spinlock_acquire(&process->lock);
		old_mask = thread->signal_mask;
		thread->signal_saved_mask = old_mask;
		thread->signal_restore_mask = 1;
		thread->signal_mask = signal_mask_sanitize(mask);
		signal_thread_mask_changed_locked(process, thread);
		spinlock_release(&process->lock);
	}
	result = vfs_poll(pollfds, count, timeout);
	if (mask_address && result != VFS_ERR_INTR) {
		spinlock_acquire(&process->lock);
		thread->signal_mask = old_mask;
		thread->signal_restore_mask = 0;
		signal_thread_mask_changed_locked(process, thread);
		spinlock_release(&process->lock);
	}
	if (result == VFS_ERR_INTR)
		return -LINUX_EINTR;
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
