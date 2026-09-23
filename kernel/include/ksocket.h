/*
 * Caffeinix socket backend interface.
 *
 * Bridges Linux IPv4 UAPI structures and VFS file descriptors to the selected
 * protocol stack.  Implementations keep stack-native descriptors private and
 * return negative Linux errno values on failure.
 */
#ifndef __CAFFEINIX_KERNEL_KSOCKET_H
#define __CAFFEINIX_KERNEL_KSOCKET_H

#include <linux_uapi.h>
#include <typedefs.h>

struct ksocket_snapshot {
	struct linux_sockaddr_in local;
	struct linux_sockaddr_in remote;
	uint64 inode;
	uint32 receive_queue;
	uint32 transmit_queue;
	uint32 uid;
	uint8 state;
};

/**
 * ksocket_init() - Initialize the socket-file registry once at boot
 *
 * Context: Early process context, before socket syscalls are enabled.
 */
void ksocket_init(void);
/**
 * ksocket_snapshot_type() - Copy procfs-style snapshots for one socket type
 * @type: Linux socket type to select.
 * @snapshots: Caller-owned output array.
 * @capacity: Number of entries in @snapshots.
 *
 * Return: Number of snapshots copied; 0 for invalid output or no matches.
 */
uint32 ksocket_snapshot_type(int type, struct ksocket_snapshot *snapshots,
			     uint32 capacity);

/**
 * ksocket_create() - Create a Linux IPv4 socket and install a VFS fd
 * @family: Supported Linux address family.
 * @type: Linux socket type, optionally containing creation flags.
 * @protocol: Linux protocol number accepted by the selected socket type.
 * @fd_out: Non-NULL destination for the installed descriptor.
 *
 * Context: Process context; may sleep in VFS or the protocol stack.
 * Return: 0 on success or a negative Linux errno.
 */
int ksocket_create(int family, int type, int protocol, int *fd_out);
/**
 * ksocket_bind() - Bind a socket to a Linux IPv4 address
 * @fd: Socket descriptor.
 * @address: Non-NULL kernel-owned IPv4 address.
 *
 * Context: Process context; may block.
 * Return: 0 or a negative Linux errno.
 */
int ksocket_bind(int fd, const struct linux_sockaddr_in *address);
/**
 * ksocket_listen() - Put a stream socket into listen state
 * @fd: Socket descriptor.
 * @backlog: Requested pending-connection limit.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_listen(int fd, int backlog);
/**
 * ksocket_accept() - Accept a connection and install its child descriptor
 * @fd: Listening socket descriptor.
 * @address: Optional kernel-owned peer-address destination.
 * @flags: Supported Linux accept4 creation flags.
 * @fd_out: Non-NULL child descriptor destination.
 *
 * Context: Process context; may sleep.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_accept(int fd, struct linux_sockaddr_in *address,
		   int flags, int *fd_out);
/**
 * ksocket_connect() - Connect or disconnect a socket
 * @fd: Socket descriptor.
 * @address: Kernel-owned IPv4 address, including AF_UNSPEC disconnect form.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_connect(int fd, const struct linux_sockaddr_in *address);
/**
 * ksocket_get_name() - Read local or peer address from a socket
 * @fd: Socket descriptor.
 * @peer: Nonzero selects peer rather than local address.
 * @address: Non-NULL kernel-owned output address.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_get_name(int fd, int peer,
		     struct linux_sockaddr_in *address);
/**
 * ksocket_send() - Send kernel memory through a socket
 * @fd: Socket descriptor.
 * @buffer: Kernel-owned source, non-NULL when @length is nonzero.
 * @length: Requested byte count.
 * @flags: Supported Linux send flags.
 * @address: Optional destination for an unconnected datagram socket.
 *
 * Context: Process context; may block.
 *
 * Return: Byte count or negative errno.
 */
int64 ksocket_send(int fd, const void *buffer, uint64 length, int flags,
		   const struct linux_sockaddr_in *address);
/**
 * ksocket_receive() - Receive one socket payload into kernel memory
 * @fd: Socket descriptor.
 * @buffer: Kernel-owned destination, non-NULL when @length is nonzero.
 * @length: Destination capacity in bytes.
 * @flags: Supported Linux receive flags.
 * @address: Optional peer-address output.
 *
 * Context: Process context; may block.
 *
 * Return: Byte count or negative errno.
 */
int64 ksocket_receive(int fd, void *buffer, uint64 length, int flags,
		      struct linux_sockaddr_in *address);
/**
 * ksocket_receive_message() - Receive payload, peer, and Linux message flags
 * @fd: Socket descriptor.
 * @buffer: Kernel-owned payload destination.
 * @length: Destination capacity in bytes.
 * @flags: Supported Linux receive flags.
 * @address: Optional peer-address output.
 * @message_flags: Optional output for Linux result flags.
 *
 * Context: Process context; may block.
 *
 * Return: Byte count or negative errno.
 */
int64 ksocket_receive_message(int fd, void *buffer, uint64 length,
			      int flags,
			      struct linux_sockaddr_in *address,
			      uint32 *message_flags);
/**
 * ksocket_set_option() - Set a supported Linux socket option
 * @fd: Socket descriptor.
 * @level: Linux option level.
 * @option: Linux option number.
 * @value: Kernel-owned option bytes.
 * @length: Size of @value in bytes.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_set_option(int fd, int level, int option,
		       const void *value, uint32 length);
/**
 * ksocket_get_option() - Read a supported Linux socket option
 * @fd: Socket descriptor.
 * @level: Linux option level.
 * @option: Linux option number.
 * @value: Kernel-owned output buffer.
 * @length: Input capacity and output byte count; must be non-NULL.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_get_option(int fd, int level, int option,
		       void *value, uint32 *length);
/**
 * ksocket_shutdown() - Disable selected socket directions
 * @fd: Socket descriptor.
 * @how: Linux shutdown direction selector.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_shutdown(int fd, int how);
/**
 * ksocket_type() - Read a socket's Linux type
 * @fd: Socket descriptor.
 * @type: Non-NULL output type destination.
 *
 * Context: Process context; may block.
 *
 * Return: 0 or a negative Linux errno.
 */
int ksocket_type(int fd, int *type);

#endif
