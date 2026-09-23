/*
 * Compatibility handles for the fixed kernel file table.
 *
 * file_t owns one VFS-file reference until file_close() or file_unhold().
 * New code should generally use the VFS descriptor and vfs_file interfaces.
 */
#ifndef __CAFFEINIX_KERNEL_FILE_H
#define __CAFFEINIX_KERNEL_FILE_H

#include <vfs.h>

#define MAXPATH VFS_PATH_MAX
#define MAXARG 32
#define NOFILE 16
#define NFILE 100

typedef struct vfs_file *file_t;

/**
 * file_init() - Initialize the fixed file table
 *
 * Context: Boot context, before concurrent users are created.
 */
void file_init(void);

/**
 * file_alloc() - Allocate an empty compatibility file slot
 *
 * Context: Thread context; does not sleep.
 *
 * Return: A referenced, zeroed file slot, or NULL when all slots are busy.
 */
file_t file_alloc(void);

/**
 * file_dup() - Take another reference to a compatibility file
 * @file: File returned by file_alloc() or file_dup().
 *
 * Context: Thread context; does not sleep.
 * Return: @file with one additional reference.  Panics for NULL or a released
 * handle.
 */
file_t file_dup(file_t file);

/**
 * file_close() - Drop one compatibility-file reference
 * @file: Live file handle with a reference to drop; NULL or released handles
 * are programming errors and panic.
 *
 * Context: Thread context; the final release may call a VFS release method.
 */
void file_close(file_t file);
/**
 * file_hold() - Take a non-access compatibility reference
 * @file: Live fixed-table handle; NULL or released handles panic.
 *
 * Context: Atomic with respect to the file table; does not sleep.
 * Return: @file with one additional reference, without an inode-access ref.
 */
file_t file_hold(file_t file);
/**
 * file_unhold() - Drop a non-access compatibility reference
 * @file: Live fixed-table handle previously retained with file_hold().
 *
 * Context: The final release may invoke backend release and path put, and may
 * sleep.  NULL or released handles panic.
 */
void file_unhold(file_t file);
/**
 * file_read() - Read through a compatibility file handle
 * @file: Referenced file handle.
 * @user_destination: Nonzero when @destination is a user address.
 * @destination: Destination address.
 * @count: Maximum byte count.
 * @position: Non-NULL in/out byte offset, forwarded to the backend unchanged.
 *
 * Context: Thread context; may sleep in the backing filesystem or device.
 * The caller serializes shared offsets. Pass &file->position explicitly to
 * use the open-file position; this wrapper does not select or lock it.
 * Return: Bytes read, including a possible short read, or a negative VFS error.
 */
int64 file_read(file_t file, int user_destination, uint64 destination,
		uint64 count, uint64 *position);
/**
 * file_write() - Write through a compatibility file handle
 * @file: Referenced file handle.
 * @user_source: Nonzero when @source is a user address.
 * @source: Source address.
 * @count: Maximum byte count.
 * @position: Non-NULL in/out byte offset, forwarded to the backend unchanged.
 *
 * Context: Thread context; may sleep in the backing filesystem or device.
 * The caller serializes shared offsets. Pass &file->position explicitly to
 * use the open-file position; this wrapper does not select or lock it.
 * Return: Bytes written, including a possible short write, or a VFS error.
 */
int64 file_write(file_t file, int user_source, uint64 source,
		 uint64 count, uint64 *position);
/**
 * file_ioctl() - Forward a device-specific control request
 * @file: Referenced file handle.
 * @request: Backend-defined command number.
 * @argument: Backend-defined scalar or user address.
 *
 * Context: Thread context; the backend determines locking and sleeping rules.
 * Return: Backend result or VFS_ERR_NOTTY if the file has no ioctl method.
 */
int64 file_ioctl(file_t file, uint64 request, uint64 argument);

#endif
