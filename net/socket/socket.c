#include <debug.h>
#include <file.h>
#include <ksocket.h>
#include <ktime.h>
#include <list.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <mystring.h>
#include <network_stack.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <sleeplock.h>
#include <signal.h>
#include <spinlock.h>
#include <vfs.h>
#include <vm.h>

#define SOCKET_LISTEN_BACKLOG_MAX 16
#define SOCKET_LINGER_MAX_SECONDS 0x7fff
#define SOCKET_RECEIVE_BUFFER_MIN 2304
#define SOCKET_RECEIVE_BUFFER_MAX 0x7fffffff
#define SOCKET_DATAGRAM_RECEIVE_MAX 0xffff
#define SOCKET_TIMEOUT_MAX_MS 0x7fffffff

#define SOCKET_INHERIT_REUSEADDR (1U << 0)
#define SOCKET_INHERIT_KEEPALIVE (1U << 1)
#define SOCKET_INHERIT_LINGER (1U << 2)
#define SOCKET_INHERIT_RCVTIMEO (1U << 3)
#define SOCKET_INHERIT_SNDTIMEO (1U << 4)
#define SOCKET_INHERIT_IP_TTL (1U << 5)
#define SOCKET_INHERIT_TCP_NODELAY (1U << 6)

struct socket_inherited_options {
	uint32 mask;
	int reuse_address;
	int keepalive;
	int ip_ttl;
	int tcp_nodelay;
	struct linger linger;
	struct timeval receive_timeout;
	struct timeval send_timeout;
};

struct socket_file {
	struct spinlock lock;
	struct list registry_node;
	int descriptor;
	int family;
	int type;
	int protocol;
	uint32 peer_address;
	uint8 has_peer;
	uint8 connect_pending;
	uint8 read_shutdown;
	uint8 write_shutdown;
	uint8 listening;
	uint8 registered;
	uint64 inode;
	uint32 uid;
	struct socket_inherited_options inherited;
};

union socket_option_storage {
	int integer;
	struct linger linger;
	struct timeval timeout;
};

static struct {
	struct sleeplock lock;
	struct list sockets;
	uint64 next_inode;
} socket_registry;

static void socket_address_from_lwip(
	const struct sockaddr_in *lwip_address,
	struct linux_sockaddr_in *linux_address);

void ksocket_init(void)
{
	sleeplock_init(&socket_registry.lock, "socket registry");
	list_init(&socket_registry.sockets);
	socket_registry.next_inode = 1;
}

static void socket_registry_add(struct socket_file *socket)
{
	sleeplock_acquire(&socket_registry.lock);
	socket->inode = socket_registry.next_inode++;
	list_insert_before(&socket_registry.sockets, &socket->registry_node);
	socket->registered = 1;
	sleeplock_release(&socket_registry.lock);
}

static void socket_registry_remove(struct socket_file *socket)
{
	sleeplock_acquire(&socket_registry.lock);
	if (socket->registered) {
		list_remove(&socket->registry_node);
		socket->registered = 0;
	}
	sleeplock_release(&socket_registry.lock);
}

static uint8 socket_snapshot_state(struct socket_file *socket)
{
	socklen_t length = sizeof(int);
	int state;

	if (socket->type != LINUX_SOCK_STREAM)
		return 0x07;
	if (lwip_getsockopt(socket->descriptor, SOL_SOCKET,
			    SO_LWIP_TCP_STATE,
			    &state, &length) < 0)
		return 0x07;
	switch (state) {
	case ESTABLISHED: return 0x01;
	case SYN_SENT: return 0x02;
	case SYN_RCVD: return 0x03;
	case FIN_WAIT_1: return 0x04;
	case FIN_WAIT_2: return 0x05;
	case TIME_WAIT: return 0x06;
	case CLOSE_WAIT: return 0x08;
	case LAST_ACK: return 0x09;
	case LISTEN: return 0x0a;
	case CLOSING: return 0x0b;
	default: return 0x07;
	}
}

uint32 ksocket_snapshot_type(int type, struct ksocket_snapshot *snapshots,
			     uint32 capacity)
{
	struct socket_file *socket;
	struct sockaddr_in address;
	list_t node;
	uint32 count = 0;

	if (!snapshots || !capacity)
		return 0;
	lwip_socket_thread_init();
	sleeplock_acquire(&socket_registry.lock);
	for (node = socket_registry.sockets.next;
	     node != &socket_registry.sockets && count < capacity;
	     node = node->next) {
		struct ksocket_snapshot *snapshot;
		socklen_t length;
		int descriptor, has_peer, queued = 0;

		socket = list_entry(node, struct socket_file, registry_node);
		spinlock_acquire(&socket->lock);
		if (socket->type != type) {
			spinlock_release(&socket->lock);
			continue;
		}
		descriptor = socket->descriptor;
		has_peer = socket->has_peer || socket->connect_pending;
		snapshot = &snapshots[count];
		memset(snapshot, 0, sizeof(*snapshot));
		snapshot->inode = socket->inode;
		snapshot->uid = socket->uid;
		spinlock_release(&socket->lock);
		snapshot->state = socket_snapshot_state(socket);

		length = sizeof(address);
		memset(&address, 0, sizeof(address));
		if (lwip_getsockname(descriptor,
				      (struct sockaddr *)&address,
				      &length) == 0)
			socket_address_from_lwip(&address, &snapshot->local);
		if (has_peer) {
			length = sizeof(address);
			memset(&address, 0, sizeof(address));
			if (lwip_getpeername(descriptor,
					     (struct sockaddr *)&address,
					     &length) == 0)
				socket_address_from_lwip(
					&address, &snapshot->remote);
		}
		if (lwip_ioctl(descriptor, FIONREAD, &queued) == 0 && queued > 0)
			snapshot->receive_queue = queued;
		count++;
	}
	sleeplock_release(&socket_registry.lock);
	return count;
}

static int socket_vfs_error(int error)
{
	switch (error) {
	case EAGAIN: return VFS_ERR_AGAIN;
	case EBADF: return VFS_ERR_BADF;
	case EFAULT: return VFS_ERR_FAULT;
	case EINVAL: return VFS_ERR_INVAL;
	case ENOMEM: return VFS_ERR_NOMEM;
	case ENFILE:
	case EMFILE: return VFS_ERR_MFILE;
	case ENOTSOCK: return VFS_ERR_NOTSOCK;
	case EDESTADDRREQ: return VFS_ERR_DESTADDRREQ;
	case EMSGSIZE: return VFS_ERR_MSGSIZE;
	case EPROTOTYPE: return VFS_ERR_PROTOTYPE;
	case ENOPROTOOPT: return VFS_ERR_NOPROTOOPT;
	case EPROTONOSUPPORT: return VFS_ERR_PROTONOSUPPORT;
	case ESOCKTNOSUPPORT: return VFS_ERR_SOCKTNOSUPPORT;
	case EOPNOTSUPP: return VFS_ERR_NOTSUPP;
	case EAFNOSUPPORT: return VFS_ERR_AFNOSUPPORT;
	case EADDRINUSE: return VFS_ERR_ADDRINUSE;
	case EADDRNOTAVAIL: return VFS_ERR_ADDRNOTAVAIL;
	case ENETDOWN: return VFS_ERR_NETDOWN;
	case ENETUNREACH: return VFS_ERR_NETUNREACH;
	case ECONNABORTED: return VFS_ERR_CONNABORTED;
	case ECONNRESET: return VFS_ERR_CONNRESET;
	case ENOBUFS: return VFS_ERR_NOBUFS;
	case EISCONN: return VFS_ERR_ISCONN;
	case ENOTCONN: return VFS_ERR_NOTCONN;
	case ESHUTDOWN: return VFS_ERR_SHUTDOWN;
	case ETIMEDOUT: return VFS_ERR_TIMEDOUT;
	case ECONNREFUSED: return VFS_ERR_CONNREFUSED;
	case EHOSTUNREACH: return VFS_ERR_HOSTUNREACH;
	case EALREADY: return VFS_ERR_ALREADY;
	case EINPROGRESS: return VFS_ERR_INPROGRESS;
	case EPIPE: return VFS_ERR_PIPE;
	default: return VFS_ERR_IO;
	}
}

static int socket_linux_error(void)
{
	return errno > 0 ? -errno : -LINUX_EIO;
}

static int socket_send_lwip_flags(int linux_flags, int *lwip_flags)
{
	const int supported = LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL |
		LINUX_MSG_MORE;
	int flags = 0;

	if (linux_flags & ~supported)
		return -LINUX_EOPNOTSUPP;
	if (linux_flags & LINUX_MSG_DONTWAIT)
		flags |= MSG_DONTWAIT;
	if (linux_flags & LINUX_MSG_MORE)
		flags |= MSG_MORE;
	*lwip_flags = flags;
	return 0;
}

static int socket_receive_lwip_flags(int linux_flags, int *lwip_flags)
{
	const int supported = LINUX_MSG_PEEK | LINUX_MSG_TRUNC |
		LINUX_MSG_DONTWAIT;
	int flags = 0;

	if (linux_flags & ~supported)
		return -LINUX_EOPNOTSUPP;
	if (linux_flags & LINUX_MSG_PEEK)
		flags |= MSG_PEEK;
	if (linux_flags & LINUX_MSG_DONTWAIT)
		flags |= MSG_DONTWAIT;
	*lwip_flags = flags;
	return 0;
}

static int socket_check_broadcast_permission(struct socket_file *socket,
					     uint32 address)
{
	socklen_t length = sizeof(int);
	int enabled;

	if ((socket->type != LINUX_SOCK_DGRAM &&
	     socket->type != LINUX_SOCK_RAW) ||
	    !network_stack_address_is_broadcast(address))
		return 0;
	errno = 0;
	if (lwip_getsockopt(socket->descriptor, SOL_SOCKET, SO_BROADCAST,
			    &enabled, &length) < 0)
		return socket_linux_error();
	return enabled ? 0 : -LINUX_EACCES;
}

static int socket_timeout_to_lwip(const void *value, uint32 length,
				  struct timeval *timeout)
{
	struct linux_timeval linux_timeout;
	uint64 milliseconds;

	if (!value || length < sizeof(linux_timeout))
		return -LINUX_EINVAL;
	memmove(&linux_timeout, value, sizeof(linux_timeout));
	if (linux_timeout.seconds < 0 || linux_timeout.microseconds < 0 ||
	    linux_timeout.microseconds >= 1000000)
		return -LINUX_EINVAL;
	if ((uint64)linux_timeout.seconds >
	    SOCKET_TIMEOUT_MAX_MS / 1000)
		milliseconds = SOCKET_TIMEOUT_MAX_MS;
	else
		milliseconds = linux_timeout.seconds * 1000 +
			(linux_timeout.microseconds + 999) / 1000;
	if (milliseconds > SOCKET_TIMEOUT_MAX_MS)
		milliseconds = SOCKET_TIMEOUT_MAX_MS;
	timeout->tv_sec = milliseconds / 1000;
	timeout->tv_usec = milliseconds % 1000 * 1000;
	return 0;
}

static void socket_timeout_from_lwip(const struct timeval *timeout,
				     struct linux_timeval *linux_timeout)
{
	linux_timeout->seconds = timeout->tv_sec;
	linux_timeout->microseconds = timeout->tv_usec;
}

static int socket_receive_buffer_to_lwip(const void *value, uint32 length,
					 int *receive_buffer)
{
	int32 requested;

	if (!value || length < sizeof(requested))
		return -LINUX_EINVAL;
	memmove(&requested, value, sizeof(requested));
	if (requested < 0 || requested >= SOCKET_RECEIVE_BUFFER_MAX / 2)
		*receive_buffer = SOCKET_RECEIVE_BUFFER_MAX;
	else
		*receive_buffer = requested * 2;
	if (*receive_buffer < SOCKET_RECEIVE_BUFFER_MIN)
		*receive_buffer = SOCKET_RECEIVE_BUFFER_MIN;
	return 0;
}

static int socket_linger_to_lwip(const void *value, uint32 length,
				  struct linger *linger)
{
	struct linux_linger requested;

	if (!value || length < sizeof(requested))
		return -LINUX_EINVAL;
	memmove(&requested, value, sizeof(requested));
	if (requested.enabled &&
	    (requested.seconds < 0 ||
	     requested.seconds > SOCKET_LINGER_MAX_SECONDS))
		return -LINUX_EINVAL;
	linger->l_onoff = !!requested.enabled;
	linger->l_linger = requested.enabled ? requested.seconds : 0;
	return 0;
}

static int socket_ip_ttl_to_lwip(const void *value, uint32 length, int *ttl)
{
	int32 requested;

	if (!value || length < sizeof(requested))
		return -LINUX_EINVAL;
	memmove(&requested, value, sizeof(requested));
	if (requested == -1)
		requested = IP_DEFAULT_TTL;
	else if (requested < 1 || requested > 255)
		return -LINUX_EINVAL;
	*ttl = requested;
	return 0;
}

static int socket_write_is_shutdown(struct socket_file *socket)
{
	return __atomic_load_n(&socket->write_shutdown, __ATOMIC_ACQUIRE);
}

static int socket_read_is_shutdown(struct socket_file *socket)
{
	return __atomic_load_n(&socket->read_shutdown, __ATOMIC_ACQUIRE);
}

static int socket_write_needs_peer(struct socket_file *socket)
{
	return (socket->type == LINUX_SOCK_DGRAM ||
		socket->type == LINUX_SOCK_RAW) &&
		!__atomic_load_n(&socket->has_peer, __ATOMIC_ACQUIRE);
}

static int socket_stream_prepare_write(struct socket_file *socket)
{
	struct sockaddr_in address;
	socklen_t address_length = sizeof(address);
	socklen_t error_length = sizeof(int);
	int error;

	if (socket->type != LINUX_SOCK_STREAM ||
	    __atomic_load_n(&socket->has_peer, __ATOMIC_ACQUIRE))
		return 0;
	if (!__atomic_load_n(&socket->connect_pending, __ATOMIC_ACQUIRE))
		return EPIPE;
	errno = 0;
	if (lwip_getpeername(socket->descriptor,
			     (struct sockaddr *)&address,
			     &address_length) == 0) {
		__atomic_store_n(&socket->has_peer, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&socket->connect_pending, 0,
				 __ATOMIC_RELEASE);
		return 0;
	}
	errno = 0;
	error = 0;
	if (lwip_getsockopt(socket->descriptor, SOL_SOCKET, SO_ERROR,
			    &error, &error_length) < 0)
		return errno > 0 ? errno : EIO;
	if (!error)
		return EAGAIN;
	__atomic_store_n(&socket->connect_pending, 0, __ATOMIC_RELEASE);
	return error;
}

static int socket_write_error(struct socket_file *socket)
{
	if (socket_write_is_shutdown(socket) ||
	    (socket->type == LINUX_SOCK_STREAM && errno == ENOTCONN))
		return EPIPE;
	return errno > 0 ? errno : EIO;
}

static int socket_report_write_error(struct socket_file *socket, int error,
				     int flags)
{
	if (error == EPIPE && socket->type == LINUX_SOCK_STREAM &&
	    !(flags & LINUX_MSG_NOSIGNAL))
		signal_raise_current(LINUX_SIGPIPE, LINUX_SI_KERNEL);
	return error;
}

static void socket_address_to_lwip(
	const struct linux_sockaddr_in *linux_address,
	struct sockaddr_in *lwip_address)
{
	memset(lwip_address, 0, sizeof(*lwip_address));
	lwip_address->sin_len = sizeof(*lwip_address);
	lwip_address->sin_family = AF_INET;
	lwip_address->sin_port = linux_address->port;
	lwip_address->sin_addr.s_addr = linux_address->address;
}

static void socket_address_from_lwip(
	const struct sockaddr_in *lwip_address,
	struct linux_sockaddr_in *linux_address)
{
	memset(linux_address, 0, sizeof(*linux_address));
	linux_address->family = LINUX_AF_INET;
	linux_address->port = lwip_address->sin_port;
	linux_address->address = lwip_address->sin_addr.s_addr;
}

static void socket_receive_address_from_lwip(
	struct socket_file *socket,
	const struct sockaddr_in *lwip_address,
	struct linux_sockaddr_in *linux_address)
{
	socket_address_from_lwip(lwip_address, linux_address);
	if (socket->type == LINUX_SOCK_RAW)
		linux_address->port = 0;
}

static void socket_remember_option(struct socket_file *socket, int level,
				   int option, const void *value)
{
	uint32 mask = 0;
	void *destination = 0;
	uint32 length = sizeof(int);

	if (level == LINUX_SOL_SOCKET) {
		switch (option) {
		case LINUX_SO_REUSEADDR:
			mask = SOCKET_INHERIT_REUSEADDR;
			destination = &socket->inherited.reuse_address;
			break;
		case LINUX_SO_KEEPALIVE:
			mask = SOCKET_INHERIT_KEEPALIVE;
			destination = &socket->inherited.keepalive;
			break;
		case LINUX_SO_LINGER:
			mask = SOCKET_INHERIT_LINGER;
			destination = &socket->inherited.linger;
			length = sizeof(struct linger);
			break;
		case LINUX_SO_RCVTIMEO:
			mask = SOCKET_INHERIT_RCVTIMEO;
			destination = &socket->inherited.receive_timeout;
			length = sizeof(struct timeval);
			break;
		case LINUX_SO_SNDTIMEO:
			mask = SOCKET_INHERIT_SNDTIMEO;
			destination = &socket->inherited.send_timeout;
			length = sizeof(struct timeval);
			break;
		}
	} else if (level == LINUX_IPPROTO_IP &&
		   option == LINUX_IP_TTL) {
		mask = SOCKET_INHERIT_IP_TTL;
		destination = &socket->inherited.ip_ttl;
	} else if (level == LINUX_IPPROTO_TCP &&
		   option == LINUX_TCP_NODELAY) {
		mask = SOCKET_INHERIT_TCP_NODELAY;
		destination = &socket->inherited.tcp_nodelay;
	}
	if (!mask)
		return;
	spinlock_acquire(&socket->lock);
	memmove(destination, value, length);
	socket->inherited.mask |= mask;
	spinlock_release(&socket->lock);
}

static void socket_snapshot_options(struct socket_file *socket,
				    struct socket_inherited_options *options)
{
	spinlock_acquire(&socket->lock);
	*options = socket->inherited;
	spinlock_release(&socket->lock);
}

static int socket_apply_lwip_option(int descriptor, int level, int option,
				    const void *value, socklen_t length)
{
	errno = 0;
	if (lwip_setsockopt(descriptor, level, option, value, length) < 0)
		return socket_linux_error();
	return 0;
}

static int socket_apply_inherited_options(
	int descriptor, const struct socket_inherited_options *options)
{
	int result;

	if (options->mask & SOCKET_INHERIT_REUSEADDR) {
		result = socket_apply_lwip_option(descriptor, SOL_SOCKET,
			SO_REUSEADDR, &options->reuse_address,
			sizeof(options->reuse_address));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_KEEPALIVE) {
		result = socket_apply_lwip_option(descriptor, SOL_SOCKET,
			SO_KEEPALIVE, &options->keepalive,
			sizeof(options->keepalive));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_LINGER) {
		result = socket_apply_lwip_option(descriptor, SOL_SOCKET,
			SO_LINGER, &options->linger,
			sizeof(options->linger));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_RCVTIMEO) {
		result = socket_apply_lwip_option(descriptor, SOL_SOCKET,
			SO_RCVTIMEO, &options->receive_timeout,
			sizeof(options->receive_timeout));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_SNDTIMEO) {
		result = socket_apply_lwip_option(descriptor, SOL_SOCKET,
			SO_SNDTIMEO, &options->send_timeout,
			sizeof(options->send_timeout));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_IP_TTL) {
		result = socket_apply_lwip_option(descriptor, IPPROTO_IP,
			IP_TTL, &options->ip_ttl,
			sizeof(options->ip_ttl));
		if (result < 0)
			return result;
	}
	if (options->mask & SOCKET_INHERIT_TCP_NODELAY) {
		result = socket_apply_lwip_option(descriptor, IPPROTO_TCP,
			TCP_NODELAY, &options->tcp_nodelay,
			sizeof(options->tcp_nodelay));
		if (result < 0)
			return result;
	}
	return 0;
}

static void socket_file_release(struct vfs_file *file)
{
	struct socket_file *socket = file->private;
	struct linger abortive = {
		.l_onoff = 1,
		.l_linger = 0,
	};

	if (!socket)
		return;
	socket_registry_remove(socket);
	lwip_socket_thread_init();
	if ((file->flags & VFS_OPEN_NONBLOCK) &&
	    (socket->inherited.mask & SOCKET_INHERIT_LINGER) &&
	    socket->inherited.linger.l_onoff &&
	    socket->inherited.linger.l_linger > 0) {
		errno = 0;
		if (lwip_fcntl(socket->descriptor, F_SETFL, 0) < 0)
			PANIC("set blocking linger close");
	}
	errno = 0;
	if (lwip_close(socket->descriptor) < 0) {
		errno = 0;
		if (lwip_setsockopt(socket->descriptor, SOL_SOCKET,
				    SO_LINGER, &abortive,
				    sizeof(abortive)) < 0)
			PANIC("set abortive socket close");
		errno = 0;
		if (lwip_close(socket->descriptor) < 0)
			PANIC("abortive socket close");
	}
	free(socket);
}

static int socket_file_set_flags(struct vfs_file *file, uint32 flags)
{
	struct socket_file *socket = file->private;
	int value = flags & VFS_OPEN_NONBLOCK ? O_NONBLOCK : 0;

	lwip_socket_thread_init();
	errno = 0;
	if (lwip_fcntl(socket->descriptor, F_SETFL, value) < 0)
		return socket_vfs_error(errno);
	return VFS_OK;
}

static int64 socket_file_read(struct vfs_file *file, int user_destination,
			      uint64 destination, uint64 count,
			      uint64 *position)
{
	struct socket_file *socket = file->private;
	void *buffer;
	int64 result;

	(void)position;
	if (!count)
		return 0;
	if (socket_read_is_shutdown(socket) &&
	    socket->type != LINUX_SOCK_STREAM)
		return 0;
	lwip_socket_thread_init();
	if (count > PGSIZE)
		count = PGSIZE;
	buffer = palloc();
	if (!buffer)
		return VFS_ERR_NOMEM;
	errno = 0;
	result = lwip_recv(socket->descriptor, buffer, count, 0);
	if (result < 0) {
		if (socket_read_is_shutdown(socket) &&
		    (errno == ENOTCONN || errno == ECONNRESET))
			result = 0;
		else
			result = socket_vfs_error(errno);
	} else if (result && either_copyout(user_destination, destination,
					buffer, result) < 0)
		result = VFS_ERR_FAULT;
	pfree(buffer);
	return result;
}

static int64 socket_file_readv(struct vfs_file *file, int user_destination,
			       const struct vfs_iovec *iovecs,
			       uint32 count)
{
	struct socket_file *socket = file->private;
	uint64 copied = 0, capacity = 0, remaining;
	unsigned int buffer_order = 0;
	void *buffer;
	uint32 index;
	int64 result;

	for (index = 0; index < count; index++)
		capacity += iovecs[index].length;
	if (!capacity)
		return 0;
	if (socket_read_is_shutdown(socket) &&
	    socket->type != LINUX_SOCK_STREAM)
		return 0;
	if (socket->type == LINUX_SOCK_STREAM && capacity > PGSIZE)
		capacity = PGSIZE;
	else if (capacity > SOCKET_DATAGRAM_RECEIVE_MAX)
		capacity = SOCKET_DATAGRAM_RECEIVE_MAX;
	remaining = capacity;
	for (index = 0; user_destination && index < count && remaining;
	     index++) {
		uint64 length = iovecs[index].length;

		if (length > remaining)
			length = remaining;
		if (length && vm_prefault_user_write(cur_proc()->pagetable,
						  iovecs[index].base,
						  length) < 0)
			return VFS_ERR_FAULT;
		remaining -= length;
	}
	while ((PGSIZE << buffer_order) < capacity)
		buffer_order++;
	buffer = alloc_pages(buffer_order, 0);
	if (!buffer)
		return VFS_ERR_NOMEM;
	lwip_socket_thread_init();
	errno = 0;
	result = lwip_recv(socket->descriptor, buffer, capacity, 0);
	if (result < 0) {
		if (socket_read_is_shutdown(socket) &&
		    (errno == ENOTCONN || errno == ECONNRESET))
			result = 0;
		else
			result = socket_vfs_error(errno);
		goto out;
	}
	for (index = 0; index < count && copied < (uint64)result; index++) {
		uint64 length = iovecs[index].length;

		if (length > (uint64)result - copied)
			length = result - copied;
		if (length && either_copyout(user_destination,
					     iovecs[index].base,
					     (uint8 *)buffer + copied,
					     length) < 0) {
			result = VFS_ERR_FAULT;
			goto out;
		}
		copied += length;
	}
out:
	free_pages(buffer, buffer_order);
	return result;
}

static int64 socket_file_write(struct vfs_file *file, int user_source,
			       uint64 source, uint64 count,
			       uint64 *position)
{
	struct socket_file *socket = file->private;
	void *buffer;
	int status;
	int64 result;

	(void)position;
	if (socket_write_is_shutdown(socket))
		return socket_vfs_error(
			socket_report_write_error(socket, EPIPE, 0));
	if (socket_write_needs_peer(socket))
		return VFS_ERR_DESTADDRREQ;
	lwip_socket_thread_init();
	status = socket_stream_prepare_write(socket);
	if (status)
		return socket_vfs_error(
			socket_report_write_error(socket, status, 0));
	if (!count && socket->type != LINUX_SOCK_DGRAM)
		return 0;
	if (!count) {
		char dummy;

		errno = 0;
		result = lwip_send(socket->descriptor, &dummy, 0, 0);
		return result < 0 ? socket_vfs_error(socket_report_write_error(
			socket, socket_write_error(socket), 0)) : result;
	}
	if (count > PGSIZE && socket->type != LINUX_SOCK_STREAM)
		return VFS_ERR_MSGSIZE;
	if (count > PGSIZE)
		count = PGSIZE;
	buffer = palloc();
	if (!buffer)
		return VFS_ERR_NOMEM;
	if (either_copyin(buffer, user_source, source, count) < 0) {
		pfree(buffer);
		return VFS_ERR_FAULT;
	}
	errno = 0;
	result = lwip_send(socket->descriptor, buffer, count, 0);
	if (result < 0)
		result = socket_vfs_error(socket_report_write_error(
			socket, socket_write_error(socket), 0));
	pfree(buffer);
	return result;
}

static int64 socket_file_writev(struct vfs_file *file, int user_source,
				const struct vfs_iovec *iovecs,
				uint32 count)
{
	struct socket_file *socket = file->private;
	uint64 copied = 0, total = 0;
	void *buffer = 0;
	uint32 index;
	int64 result;

	for (index = 0; index < count; index++)
		total += iovecs[index].length;
	if (!total)
		return 0;
	if (socket_write_is_shutdown(socket))
		return socket_vfs_error(
			socket_report_write_error(socket, EPIPE, 0));
	if (socket_write_needs_peer(socket))
		return VFS_ERR_DESTADDRREQ;
	lwip_socket_thread_init();
	result = socket_stream_prepare_write(socket);
	if (result)
		return socket_vfs_error(
			socket_report_write_error(socket, result, 0));
	if (socket->type != LINUX_SOCK_STREAM && total > PGSIZE)
		return VFS_ERR_MSGSIZE;
	if (total > PGSIZE)
		total = PGSIZE;
	if (total) {
		buffer = palloc();
		if (!buffer)
			return VFS_ERR_NOMEM;
		for (index = 0; index < count && copied < total; index++) {
			uint64 length = iovecs[index].length;

			if (length > total - copied)
				length = total - copied;
			if (length && either_copyin((uint8 *)buffer + copied,
						 user_source, iovecs[index].base,
						 length) < 0) {
				pfree(buffer);
				return VFS_ERR_FAULT;
			}
			copied += length;
		}
	}
	errno = 0;
	result = lwip_send(socket->descriptor, buffer, total, 0);
	if (result < 0)
		result = socket_vfs_error(socket_report_write_error(
			socket, socket_write_error(socket), 0));
	if (buffer)
		pfree(buffer);
	return result;
}

static int64 socket_file_ioctl(struct vfs_file *file, uint64 request,
			       uint64 argument)
{
	struct socket_file *socket = file->private;
	int value, status;

	lwip_socket_thread_init();
	if (request == LINUX_FIONBIO) {
		if (either_copyin(&value, 1, argument, sizeof(value)) < 0)
			return VFS_ERR_FAULT;
		status = socket_file_set_flags(file,
			(value ? file->flags | VFS_OPEN_NONBLOCK :
			 file->flags & ~VFS_OPEN_NONBLOCK));
		if (status < 0)
			return status;
		if (value)
			file->flags |= VFS_OPEN_NONBLOCK;
		else
			file->flags &= ~VFS_OPEN_NONBLOCK;
		return 0;
	}
	if (request == LINUX_FIONREAD) {
		errno = 0;
		if (lwip_ioctl(socket->descriptor, FIONREAD, &value) < 0)
			return socket_vfs_error(errno);
		if (either_copyout(1, argument, &value, sizeof(value)) < 0)
			return VFS_ERR_FAULT;
		return 0;
	}
	return VFS_ERR_NOTTY;
}

static int socket_file_getattr(struct vfs_file *file,
			       struct vfs_stat *stat)
{
	struct socket_file *socket = file ? file->private : 0;

	if (!socket || !stat)
		return VFS_ERR_INVAL;
	memset(stat, 0, sizeof(*stat));
	spinlock_acquire(&socket->lock);
	stat->ino = socket->inode;
	stat->uid = socket->uid;
	spinlock_release(&socket->lock);
	stat->type = VFS_INODE_SOCKET;
	stat->mode = 0666;
	stat->nlink = 1;
	stat->block_size = 1;
	return VFS_OK;
}

static uint32 socket_file_poll(struct vfs_file *file, uint32 events)
{
	struct socket_file *socket = file->private;
	struct pollfd pollfd = {
		.fd = socket->descriptor,
	};
	uint32 ready = 0;

	lwip_socket_thread_init();
	if (events & VFS_POLL_IN)
		pollfd.events |= POLLIN;
	if (events & VFS_POLL_OUT)
		pollfd.events |= POLLOUT;
	errno = 0;
	if (lwip_poll(&pollfd, 1, 0) < 0)
		return VFS_POLL_ERR;
	if (pollfd.revents & POLLIN)
		ready |= VFS_POLL_IN;
	if (pollfd.revents & POLLOUT)
		ready |= VFS_POLL_OUT;
	if (pollfd.revents & POLLERR)
		ready |= VFS_POLL_ERR;
	if (pollfd.revents & POLLHUP)
		ready |= VFS_POLL_HUP;
	if (pollfd.revents & POLLNVAL)
		ready |= VFS_POLL_NVAL;
	if ((events & VFS_POLL_IN) && socket_read_is_shutdown(socket))
		ready |= VFS_POLL_IN;
	if (socket_read_is_shutdown(socket) &&
	    socket_write_is_shutdown(socket))
		ready |= VFS_POLL_HUP;
	return ready;
}

static const struct vfs_file_operations socket_file_operations = {
	.release = socket_file_release,
	.read = socket_file_read,
	.readv = socket_file_readv,
	.write = socket_file_write,
	.writev = socket_file_writev,
	.ioctl = socket_file_ioctl,
	.getattr = socket_file_getattr,
	.set_flags = socket_file_set_flags,
	.poll = socket_file_poll,
};

static int socket_install(int descriptor, int family, int type,
			  int protocol,
			  const struct socket_inherited_options *inherited,
			  int connected, int flags, int *fd_out)
{
	struct process_credentials credentials;
	struct socket_file *socket;
	file_t file;
	int status;

	socket = calloc(1, sizeof(*socket));
	if (!socket) {
		lwip_close(descriptor);
		return -LINUX_ENOMEM;
	}
	file = file_alloc();
	if (!file) {
		free(socket);
		lwip_close(descriptor);
		return -LINUX_ENFILE;
	}
	socket->descriptor = descriptor;
	socket->family = family;
	socket->type = type;
	socket->protocol = protocol;
	socket->has_peer = connected;
	process_credentials_get(&credentials);
	socket->uid = credentials.fsuid;
	spinlock_init(&socket->lock, "socket options");
	list_init(&socket->registry_node);
	if (inherited)
		socket->inherited = *inherited;
	file->operations = &socket_file_operations;
	file->private = socket;
	file->flags = VFS_OPEN_READ | VFS_OPEN_WRITE;
	socket_registry_add(socket);
	if (flags & LINUX_SOCK_NONBLOCK) {
		int error;

		file->flags |= VFS_OPEN_NONBLOCK;
		status = socket_file_set_flags(file, file->flags);
		if (status < 0) {
			error = errno;
			file_close(file);
			return error ? -error : -LINUX_EIO;
		}
	}
	status = vfs_install_file(file,
		flags & LINUX_SOCK_CLOEXEC ? VFS_FD_CLOEXEC : 0, fd_out);
	if (status < 0) {
		file_close(file);
		return status == VFS_ERR_MFILE ?
			-LINUX_EMFILE : -LINUX_EINVAL;
	}
	return 0;
}

static int socket_get(int fd, file_t *file_out,
		      struct socket_file **socket_out)
{
	file_t file;
	int status;

	lwip_socket_thread_init();
	status = vfs_get_file_fd(fd, &file);
	if (status < 0)
		return -LINUX_EBADF;
	if (file->operations != &socket_file_operations || !file->private) {
		vfs_file_put(file);
		return -LINUX_ENOTSOCK;
	}
	*file_out = file;
	*socket_out = file->private;
	return 0;
}

int ksocket_create(int family, int type, int protocol, int *fd_out)
{
	struct process_credentials credentials;
	int base_type = type & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
	int descriptor;

	if (!fd_out)
		return -LINUX_EINVAL;
	if (family != LINUX_AF_INET)
		return -LINUX_EAFNOSUPPORT;
	if (base_type != LINUX_SOCK_STREAM &&
	    base_type != LINUX_SOCK_DGRAM &&
	    base_type != LINUX_SOCK_RAW)
		return -LINUX_ESOCKTNOSUPPORT;
	if (type & ~(base_type | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
		return -LINUX_EINVAL;
	if (base_type == LINUX_SOCK_RAW) {
		process_credentials_get(&credentials);
		if (credentials.euid)
			return -LINUX_EPERM;
		if (protocol != LINUX_IPPROTO_ICMP)
			return -LINUX_EPROTONOSUPPORT;
	}
	if ((base_type == LINUX_SOCK_STREAM &&
	     protocol != LINUX_IPPROTO_IP &&
	     protocol != LINUX_IPPROTO_TCP) ||
	    (base_type == LINUX_SOCK_DGRAM &&
	     protocol != LINUX_IPPROTO_IP &&
	     protocol != LINUX_IPPROTO_UDP))
		return -LINUX_EPROTONOSUPPORT;
	lwip_socket_thread_init();
	errno = 0;
	descriptor = lwip_socket(AF_INET, base_type, protocol);
	if (descriptor < 0)
		return socket_linux_error();
	return socket_install(descriptor, family, base_type, protocol, 0, 0,
			      type, fd_out);
}

int ksocket_bind(int fd, const struct linux_sockaddr_in *address)
{
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	file_t file;
	int result;

	if (!address || address->family != LINUX_AF_INET)
		return -LINUX_EAFNOSUPPORT;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	socket_address_to_lwip(address, &lwip_address);
	errno = 0;
	result = lwip_bind(socket->descriptor,
		(const struct sockaddr *)&lwip_address, sizeof(lwip_address));
	if (result < 0)
		result = socket_linux_error();
	vfs_file_put(file);
	return result;
}

int ksocket_listen(int fd, int backlog)
{
	struct socket_file *socket;
	file_t file;
	int result = socket_get(fd, &file, &socket);

	if (result < 0)
		return result;
	if (backlog < 0)
		backlog = 0;
	if (backlog > SOCKET_LISTEN_BACKLOG_MAX)
		backlog = SOCKET_LISTEN_BACKLOG_MAX;
	errno = 0;
	result = lwip_listen(socket->descriptor, backlog);
	if (result < 0)
		result = socket_linux_error();
	else {
		spinlock_acquire(&socket->lock);
		socket->listening = 1;
		spinlock_release(&socket->lock);
	}
	vfs_file_put(file);
	return result;
}

int ksocket_accept(int fd, struct linux_sockaddr_in *address,
		   int flags, int *fd_out)
{
	struct socket_inherited_options inherited;
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	socklen_t length = sizeof(lwip_address);
	file_t file;
	int descriptor, result;

	if (flags & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
		return -LINUX_EINVAL;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	errno = 0;
	descriptor = lwip_accept(socket->descriptor,
		address ? (struct sockaddr *)&lwip_address : 0,
		address ? &length : 0);
	if (descriptor < 0) {
		result = socket_linux_error();
		vfs_file_put(file);
		return result;
	}
	socket_snapshot_options(socket, &inherited);
	result = socket_apply_inherited_options(descriptor, &inherited);
	if (result < 0) {
		errno = 0;
		if (lwip_close(descriptor) < 0)
			PANIC("close rejected accepted socket");
		vfs_file_put(file);
		return result;
	}
	if (address)
		socket_address_from_lwip(&lwip_address, address);
	result = socket_install(descriptor, socket->family, socket->type,
				socket->protocol, &inherited, 1, flags, fd_out);
	vfs_file_put(file);
	return result;
}

int ksocket_connect(int fd, const struct linux_sockaddr_in *address)
{
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	file_t file;
	int result;

	if (!address || (address->family != LINUX_AF_INET &&
			 address->family != LINUX_AF_UNSPEC))
		return -LINUX_EAFNOSUPPORT;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	if (address->family == LINUX_AF_INET) {
		result = socket_check_broadcast_permission(
			socket, address->address);
		if (result < 0) {
			vfs_file_put(file);
			return result;
		}
	}
	if (address->family == LINUX_AF_UNSPEC) {
		if (socket->type != LINUX_SOCK_DGRAM &&
		    socket->type != LINUX_SOCK_RAW) {
			vfs_file_put(file);
			return -LINUX_EAFNOSUPPORT;
		}
		memset(&lwip_address, 0, sizeof(lwip_address));
		lwip_address.sin_len = sizeof(lwip_address);
		lwip_address.sin_family = AF_UNSPEC;
	} else {
		socket_address_to_lwip(address, &lwip_address);
	}
	errno = 0;
	result = lwip_connect(socket->descriptor,
		(const struct sockaddr *)&lwip_address, sizeof(lwip_address));
	if (result < 0) {
		if (socket->type == LINUX_SOCK_STREAM) {
			if (errno == EINPROGRESS || errno == EALREADY)
				__atomic_store_n(&socket->connect_pending, 1,
						 __ATOMIC_RELEASE);
			else
				__atomic_store_n(&socket->connect_pending, 0,
						 __ATOMIC_RELEASE);
		}
		result = socket_linux_error();
	} else if (socket->type == LINUX_SOCK_STREAM) {
		__atomic_store_n(&socket->has_peer, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&socket->connect_pending, 0,
				 __ATOMIC_RELEASE);
	} else if (socket->type == LINUX_SOCK_DGRAM ||
		 socket->type == LINUX_SOCK_RAW) {
		if (address->family == LINUX_AF_INET)
			__atomic_store_n(&socket->peer_address,
					 address->address, __ATOMIC_RELAXED);
		else
			__atomic_store_n(&socket->peer_address, 0,
					 __ATOMIC_RELAXED);
		__atomic_store_n(&socket->has_peer,
				 address->family == LINUX_AF_INET,
				 __ATOMIC_RELEASE);
	}
	vfs_file_put(file);
	return result;
}

int ksocket_get_name(int fd, int peer,
		     struct linux_sockaddr_in *address)
{
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	socklen_t length = sizeof(lwip_address);
	file_t file;
	int result;

	if (!address)
		return -LINUX_EFAULT;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	errno = 0;
	if (peer)
		result = lwip_getpeername(socket->descriptor,
			(struct sockaddr *)&lwip_address, &length);
	else
		result = lwip_getsockname(socket->descriptor,
			(struct sockaddr *)&lwip_address, &length);
	if (result < 0)
		result = socket_linux_error();
	else
		socket_address_from_lwip(&lwip_address, address);
	vfs_file_put(file);
	return result;
}

int64 ksocket_send(int fd, const void *buffer, uint64 length, int flags,
		   const struct linux_sockaddr_in *address)
{
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	file_t file;
	int lwip_flags, status;
	int64 result;

	status = socket_send_lwip_flags(flags, &lwip_flags);
	if (status < 0)
		return status;
	if (address && address->family != LINUX_AF_INET)
		return -LINUX_EAFNOSUPPORT;
	status = socket_get(fd, &file, &socket);
	if (status < 0)
		return status;
	if ((flags & LINUX_MSG_MORE) &&
	    socket->type != LINUX_SOCK_STREAM) {
		vfs_file_put(file);
		return -LINUX_EOPNOTSUPP;
	}
	if (socket_write_is_shutdown(socket)) {
		status = socket_report_write_error(socket, EPIPE, flags);
		vfs_file_put(file);
		return -status;
	}
	if (!address && socket_write_needs_peer(socket)) {
		vfs_file_put(file);
		return -LINUX_EDESTADDRREQ;
	}
	status = socket_stream_prepare_write(socket);
	if (status) {
		status = socket_report_write_error(socket, status, flags);
		vfs_file_put(file);
		return -status;
	}
	status = socket_check_broadcast_permission(
		socket, address ? address->address :
		__atomic_load_n(&socket->peer_address, __ATOMIC_RELAXED));
	if (status < 0) {
		vfs_file_put(file);
		return status;
	}
	errno = 0;
	if (address) {
		socket_address_to_lwip(address, &lwip_address);
		result = lwip_sendto(socket->descriptor, buffer, length,
			lwip_flags, (const struct sockaddr *)&lwip_address,
			sizeof(lwip_address));
	} else {
		result = lwip_send(socket->descriptor, buffer, length,
				   lwip_flags);
	}
	if (result < 0)
		result = -socket_report_write_error(
			socket, socket_write_error(socket), flags);
	vfs_file_put(file);
	return result;
}

int64 ksocket_receive_message(int fd, void *buffer, uint64 length,
			      int flags,
			      struct linux_sockaddr_in *address,
			      uint32 *message_flags)
{
	struct sockaddr_in lwip_address;
	struct socket_file *socket;
	socklen_t address_length = sizeof(lwip_address);
	file_t file;
	int lwip_flags, status;
	int64 result;

	status = socket_receive_lwip_flags(flags, &lwip_flags);
	if (status < 0)
		return status;
	status = socket_get(fd, &file, &socket);
	if (status < 0)
		return status;
	if (message_flags)
		*message_flags = 0;
	if (socket_read_is_shutdown(socket) &&
	    (socket->type != LINUX_SOCK_STREAM || !length)) {
		if (address)
			memset(address, 0, sizeof(*address));
		vfs_file_put(file);
		return 0;
	}
	if (socket->type == LINUX_SOCK_STREAM && !length) {
		if (address) {
			errno = 0;
			status = lwip_getpeername(socket->descriptor,
				(struct sockaddr *)&lwip_address,
				&address_length);
			if (status < 0)
				status = socket_linux_error();
			else
				socket_address_from_lwip(&lwip_address,
						 address);
		} else {
			status = 0;
		}
		vfs_file_put(file);
		return status;
	}
	errno = 0;
	if (socket->type == LINUX_SOCK_STREAM) {
		char dummy;
		void *receive_buffer = length ? buffer : &dummy;

		if (address)
			result = lwip_recvfrom(socket->descriptor, receive_buffer,
				length,
				lwip_flags, (struct sockaddr *)&lwip_address,
				&address_length);
		else
			result = lwip_recv(socket->descriptor, receive_buffer,
					   length, lwip_flags);
	} else {
		char dummy;
		struct iovec iovec = {
			.iov_base = length ? buffer : &dummy,
			.iov_len = length ? length : 1,
		};
		struct msghdr message = {
			.msg_name = address ? &lwip_address : 0,
			.msg_namelen = address ? sizeof(lwip_address) : 0,
			.msg_iov = &iovec,
			.msg_iovlen = 1,
		};

		result = lwip_recvmsg(socket->descriptor, &message, lwip_flags);
		address_length = message.msg_namelen;
		if (message_flags && (message.msg_flags & MSG_TRUNC))
			*message_flags |= LINUX_MSG_TRUNC;
		if (result > (int64)length) {
			if (message_flags)
				*message_flags |= LINUX_MSG_TRUNC;
			if (!(flags & LINUX_MSG_TRUNC))
				result = length;
		}
	}
	if (result < 0) {
		if (socket_read_is_shutdown(socket) &&
		    (errno == ENOTCONN || errno == ECONNRESET))
			result = 0;
		else
			result = socket_linux_error();
	} else if (address)
		socket_receive_address_from_lwip(socket, &lwip_address,
					 address);
	vfs_file_put(file);
	return result;
}

int64 ksocket_receive(int fd, void *buffer, uint64 length, int flags,
		      struct linux_sockaddr_in *address)
{
	return ksocket_receive_message(fd, buffer, length, flags, address, 0);
}

static int socket_option(int linux_level, int linux_option,
			 int *lwip_level, int *lwip_option)
{
	if (linux_level == LINUX_SOL_SOCKET) {
		*lwip_level = SOL_SOCKET;
		switch (linux_option) {
		case LINUX_SO_REUSEADDR: *lwip_option = SO_REUSEADDR; break;
		case LINUX_SO_TYPE: *lwip_option = SO_TYPE; break;
		case LINUX_SO_ERROR: *lwip_option = SO_ERROR; break;
		case LINUX_SO_BROADCAST: *lwip_option = SO_BROADCAST; break;
		case LINUX_SO_RCVBUF: *lwip_option = SO_RCVBUF; break;
		case LINUX_SO_KEEPALIVE: *lwip_option = SO_KEEPALIVE; break;
		case LINUX_SO_LINGER: *lwip_option = SO_LINGER; break;
		case LINUX_SO_RCVTIMEO: *lwip_option = SO_RCVTIMEO; break;
		case LINUX_SO_SNDTIMEO: *lwip_option = SO_SNDTIMEO; break;
		case LINUX_SO_ACCEPTCONN: *lwip_option = SO_ACCEPTCONN; break;
		default: return -LINUX_ENOPROTOOPT;
		}
		return 0;
	}
	if (linux_level == LINUX_IPPROTO_IP &&
	    linux_option == LINUX_IP_TTL) {
		*lwip_level = IPPROTO_IP;
		*lwip_option = IP_TTL;
		return 0;
	}
	if (linux_level == LINUX_IPPROTO_TCP &&
	    linux_option == LINUX_TCP_NODELAY) {
		*lwip_level = IPPROTO_TCP;
		*lwip_option = TCP_NODELAY;
		return 0;
	}
	return -LINUX_ENOPROTOOPT;
}

static uint32 socket_lwip_option_size(int level, int option)
{
	if (level == LINUX_SOL_SOCKET) {
		if (option == LINUX_SO_LINGER)
			return sizeof(struct linger);
		if (option == LINUX_SO_RCVTIMEO ||
		    option == LINUX_SO_SNDTIMEO)
			return sizeof(struct timeval);
	}
	return sizeof(int);
}

int ksocket_set_option(int fd, int level, int option,
		       const void *value, uint32 length)
{
	struct linger linger;
	struct timeval timeout;
	struct socket_file *socket;
	const void *lwip_value = value;
	file_t file;
	uint32 lwip_length = length;
	int listening, receive_buffer, tcp_nodelay, ttl;
	int lwip_level, lwip_option, result;

	result = socket_option(level, option, &lwip_level, &lwip_option);
	if (result < 0)
		return result;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	if (level == LINUX_SOL_SOCKET && option == LINUX_SO_RCVBUF &&
	    socket->type == LINUX_SOCK_STREAM) {
		vfs_file_put(file);
		return -LINUX_ENOPROTOOPT;
	}
	if (level == LINUX_SOL_SOCKET && option == LINUX_SO_RCVBUF) {
		result = socket_receive_buffer_to_lwip(value, length,
						       &receive_buffer);
		if (result < 0) {
			vfs_file_put(file);
			return result;
		}
		lwip_value = &receive_buffer;
		lwip_length = sizeof(receive_buffer);
	}
	if (level == LINUX_SOL_SOCKET &&
	    (option == LINUX_SO_RCVTIMEO ||
	     option == LINUX_SO_SNDTIMEO)) {
		result = socket_timeout_to_lwip(value, length, &timeout);
		if (result < 0) {
			vfs_file_put(file);
			return result;
		}
		lwip_value = &timeout;
		lwip_length = sizeof(timeout);
	}
	if (level == LINUX_SOL_SOCKET && option == LINUX_SO_LINGER) {
		result = socket_linger_to_lwip(value, length, &linger);
		if (result < 0) {
			vfs_file_put(file);
			return result;
		}
		lwip_value = &linger;
		lwip_length = sizeof(linger);
	}
	if (level == LINUX_IPPROTO_IP && option == LINUX_IP_TTL) {
		result = socket_ip_ttl_to_lwip(value, length, &ttl);
		if (result < 0) {
			vfs_file_put(file);
			return result;
		}
		lwip_value = &ttl;
		lwip_length = sizeof(ttl);
	}
	if (level == LINUX_IPPROTO_TCP &&
	    option == LINUX_TCP_NODELAY) {
		if (!value || length < sizeof(tcp_nodelay)) {
			vfs_file_put(file);
			return -LINUX_EINVAL;
		}
		memmove(&tcp_nodelay, value, sizeof(tcp_nodelay));
		tcp_nodelay = !!tcp_nodelay;
		lwip_value = &tcp_nodelay;
		lwip_length = sizeof(tcp_nodelay);
		spinlock_acquire(&socket->lock);
		listening = socket->listening;
		spinlock_release(&socket->lock);
		if (listening) {
			socket_remember_option(socket, level, option,
					       lwip_value);
			vfs_file_put(file);
			return 0;
		}
	}
	errno = 0;
	result = lwip_setsockopt(socket->descriptor, lwip_level,
				 lwip_option, lwip_value, lwip_length);
	if (result < 0)
		result = socket_linux_error();
	else
		socket_remember_option(socket, level, option, lwip_value);
	vfs_file_put(file);
	return result;
}

int ksocket_get_option(int fd, int level, int option,
		       void *value, uint32 *length)
{
	struct socket_inherited_options inherited;
	union socket_option_storage storage;
	struct linux_timeval linux_timeout;
	struct socket_file *socket;
	const void *linux_value;
	socklen_t lwip_length;
	file_t file;
	uint32 capacity, linux_length;
	int listening, timeout_option;
	int lwip_level, lwip_option, result;

	if (!value || !length)
		return -LINUX_EFAULT;
	capacity = *length;
	result = socket_option(level, option, &lwip_level, &lwip_option);
	if (result < 0)
		return result;
	result = socket_get(fd, &file, &socket);
	if (result < 0)
		return result;
	if (level == LINUX_IPPROTO_TCP &&
	    option == LINUX_TCP_NODELAY) {
		spinlock_acquire(&socket->lock);
		listening = socket->listening;
		spinlock_release(&socket->lock);
		if (listening) {
			socket_snapshot_options(socket, &inherited);
			storage.integer =
				inherited.mask & SOCKET_INHERIT_TCP_NODELAY ?
				inherited.tcp_nodelay : 0;
			linux_length = sizeof(storage.integer);
			if (linux_length > capacity)
				linux_length = capacity;
			if (linux_length)
				memmove(value, &storage.integer, linux_length);
			*length = linux_length;
			vfs_file_put(file);
			return 0;
		}
	}
	if (level == LINUX_SOL_SOCKET && option == LINUX_SO_RCVBUF &&
	    socket->type == LINUX_SOCK_STREAM) {
		vfs_file_put(file);
		return -LINUX_ENOPROTOOPT;
	}
	timeout_option = level == LINUX_SOL_SOCKET &&
		(option == LINUX_SO_RCVTIMEO ||
		 option == LINUX_SO_SNDTIMEO);
	lwip_length = socket_lwip_option_size(level, option);
	memset(&storage, 0, sizeof(storage));
	linux_value = &storage;
	linux_length = 0;
	errno = 0;
	result = lwip_getsockopt(socket->descriptor, lwip_level,
				 lwip_option, &storage, &lwip_length);
	if (result < 0)
		result = socket_linux_error();
	else if (timeout_option) {
		socket_timeout_from_lwip(&storage.timeout, &linux_timeout);
		linux_value = &linux_timeout;
		linux_length = sizeof(linux_timeout);
	} else {
		linux_value = &storage;
		linux_length = lwip_length;
	}
	if (result >= 0) {
		if (linux_length > capacity)
			linux_length = capacity;
		if (linux_length)
			memmove(value, linux_value, linux_length);
		*length = linux_length;
	}
	vfs_file_put(file);
	return result;
}

int ksocket_shutdown(int fd, int how)
{
	struct socket_file *socket;
	file_t file;
	int result = socket_get(fd, &file, &socket);

	if (result < 0)
		return result;
	if (how < LINUX_SHUT_RD || how > LINUX_SHUT_RDWR) {
		vfs_file_put(file);
		return -LINUX_EINVAL;
	}
	if (socket->type != LINUX_SOCK_STREAM &&
	    !__atomic_load_n(&socket->has_peer, __ATOMIC_ACQUIRE)) {
		vfs_file_put(file);
		return -LINUX_ENOTCONN;
	}
	if (socket->type == LINUX_SOCK_STREAM) {
		errno = 0;
		result = lwip_shutdown(socket->descriptor, how);
		if (result < 0)
			result = socket_linux_error();
	} else {
		result = 0;
	}
	if (result >= 0 && how != LINUX_SHUT_WR)
		__atomic_store_n(&socket->read_shutdown, 1,
				 __ATOMIC_RELEASE);
	if (result >= 0 && how != LINUX_SHUT_RD)
		__atomic_store_n(&socket->write_shutdown, 1,
				 __ATOMIC_RELEASE);
	vfs_file_put(file);
	return result;
}

int ksocket_type(int fd, int *type)
{
	struct socket_file *socket;
	file_t file;
	int result = socket_get(fd, &file, &socket);

	if (result < 0)
		return result;
	*type = socket->type;
	vfs_file_put(file);
	return 0;
}
