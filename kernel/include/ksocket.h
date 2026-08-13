#ifndef __CAFFEINIX_KERNEL_KSOCKET_H
#define __CAFFEINIX_KERNEL_KSOCKET_H

#include <linux_uapi.h>
#include <typedefs.h>

int ksocket_create(int family, int type, int protocol, int *fd_out);
int ksocket_bind(int fd, const struct linux_sockaddr_in *address);
int ksocket_listen(int fd, int backlog);
int ksocket_accept(int fd, struct linux_sockaddr_in *address,
		   int flags, int *fd_out);
int ksocket_connect(int fd, const struct linux_sockaddr_in *address);
int ksocket_get_name(int fd, int peer,
		     struct linux_sockaddr_in *address);
int64 ksocket_send(int fd, const void *buffer, uint64 length, int flags,
		   const struct linux_sockaddr_in *address);
int64 ksocket_receive(int fd, void *buffer, uint64 length, int flags,
		      struct linux_sockaddr_in *address);
int64 ksocket_receive_message(int fd, void *buffer, uint64 length,
			      int flags,
			      struct linux_sockaddr_in *address,
			      uint32 *message_flags);
int ksocket_set_option(int fd, int level, int option,
		       const void *value, uint32 length);
int ksocket_get_option(int fd, int level, int option,
		       void *value, uint32 *length);
int ksocket_shutdown(int fd, int how);
int ksocket_type(int fd, int *type);

#endif
