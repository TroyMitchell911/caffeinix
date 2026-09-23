/*
 * VFS object model and filesystem-facing contracts.
 *
 * Paths own a mount and dentry reference. Files with a path own its references;
 * inodes only borrow their superblock pointer. The mount/filesystem must keep
 * that superblock alive until all its inodes are gone. Callers must put every
 * owned reference acquired by a get/open function. Filesystem methods return
 * VFS_ERR_* values rather than Linux errno; syscall translation is deliberately
 * outside this API.
 */
#ifndef __CAFFEINIX_KERNEL_VFS_H
#define __CAFFEINIX_KERNEL_VFS_H

#include <block_device.h>
#include <sleeplock.h>
#include <typedefs.h>

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 512

#define VFS_OPEN_READ       (1U << 0)
#define VFS_OPEN_WRITE      (1U << 1)
#define VFS_OPEN_CREATE     (1U << 2)
#define VFS_OPEN_EXCLUSIVE  (1U << 3)
#define VFS_OPEN_TRUNCATE   (1U << 4)
#define VFS_OPEN_DIRECTORY  (1U << 5)
#define VFS_OPEN_APPEND     (1U << 6)
#define VFS_OPEN_NONBLOCK   (1U << 7)
#define VFS_OPEN_NOCTTY     (1U << 8)
#define VFS_OPEN_EXEC       (1U << 9)

#define VFS_MOUNT_NOATIME    (1U << 0)
#define VFS_MOUNT_NODIRATIME (1U << 1)
#define VFS_MOUNT_RELATIME   (1U << 2)
#define VFS_MOUNT_STRICTATIME (1U << 3)
#define VFS_MOUNT_ATIME_MASK \
	(VFS_MOUNT_NOATIME | VFS_MOUNT_RELATIME | \
	 VFS_MOUNT_STRICTATIME)

#define VFS_ACCESS_EXEC  (1U << 0)
#define VFS_ACCESS_WRITE (1U << 1)
#define VFS_ACCESS_READ  (1U << 2)

#define VFS_WRITE_NOAPPEND  (1U << 0)

#define VFS_FD_CLOEXEC      (1U << 0)

#define VFS_POLL_IN   0x001
#define VFS_POLL_OUT  0x004
#define VFS_POLL_ERR  0x008
#define VFS_POLL_HUP  0x010
#define VFS_POLL_NVAL 0x020

#define VFS_MODE_PERMISSIONS 07777U

#define VFS_TIME_ATIME (1U << 0)
#define VFS_TIME_MTIME (1U << 1)

#define VFS_ATTR_MODE (1U << 0)
#define VFS_ATTR_UID  (1U << 1)
#define VFS_ATTR_GID  (1U << 2)

#define VFS_MAKE_DEVICE(major, minor) \
	(((uint64)(uint32)(major) << 32) | (uint32)(minor))
#define VFS_DEVICE_MAJOR(device) ((uint32)((device) >> 32))
#define VFS_DEVICE_MINOR(device) ((uint32)(device))

enum vfs_result {
	VFS_OK = 0,
	VFS_ERR_PERM = -1,
	VFS_ERR_NOENT = -2,
	VFS_ERR_IO = -3,
	VFS_ERR_BADF = -4,
	VFS_ERR_EXIST = -5,
	VFS_ERR_NOTDIR = -6,
	VFS_ERR_ISDIR = -7,
	VFS_ERR_INVAL = -8,
	VFS_ERR_MFILE = -9,
	VFS_ERR_NOSPC = -10,
	VFS_ERR_NOTEMPTY = -11,
	VFS_ERR_NODEV = -12,
	VFS_ERR_NOMEM = -13,
	VFS_ERR_NOTSUPP = -14,
	VFS_ERR_NAMETOOLONG = -15,
	VFS_ERR_BUSY = -16,
	VFS_ERR_LOOP = -17,
	VFS_ERR_XDEV = -18,
	VFS_ERR_MLINK = -19,
	VFS_ERR_NOTTY = -20,
	VFS_ERR_NXIO = -21,
	VFS_ERR_FAULT = -22,
	VFS_ERR_AGAIN = -23,
	VFS_ERR_SPIPE = -24,
	VFS_ERR_NOTSOCK = -25,
	VFS_ERR_DESTADDRREQ = -26,
	VFS_ERR_MSGSIZE = -27,
	VFS_ERR_PROTOTYPE = -28,
	VFS_ERR_NOPROTOOPT = -29,
	VFS_ERR_PROTONOSUPPORT = -30,
	VFS_ERR_SOCKTNOSUPPORT = -31,
	VFS_ERR_AFNOSUPPORT = -32,
	VFS_ERR_ADDRINUSE = -33,
	VFS_ERR_ADDRNOTAVAIL = -34,
	VFS_ERR_NETDOWN = -35,
	VFS_ERR_NETUNREACH = -36,
	VFS_ERR_CONNABORTED = -37,
	VFS_ERR_CONNRESET = -38,
	VFS_ERR_NOBUFS = -39,
	VFS_ERR_ISCONN = -40,
	VFS_ERR_NOTCONN = -41,
	VFS_ERR_SHUTDOWN = -42,
	VFS_ERR_TIMEDOUT = -43,
	VFS_ERR_CONNREFUSED = -44,
	VFS_ERR_HOSTUNREACH = -45,
	VFS_ERR_ALREADY = -46,
	VFS_ERR_INPROGRESS = -47,
	VFS_ERR_PIPE = -48,
	VFS_ERR_INTR = -49,
	VFS_ERR_OVERFLOW = -50,
	VFS_ERR_ACCES = -51,
	VFS_ERR_TXTBSY = -52,
};

#define VFS_RENAME_NOREPLACE (1U << 0)

enum vfs_inode_type {
	VFS_INODE_NONE = 0,
	VFS_INODE_REGULAR,
	VFS_INODE_DIRECTORY,
	VFS_INODE_CHAR_DEVICE,
	VFS_INODE_BLOCK_DEVICE,
	VFS_INODE_SYMLINK,
	VFS_INODE_FIFO,
	VFS_INODE_SOCKET,
};

enum vfs_dirent_type {
	VFS_DT_UNKNOWN = 0,
	VFS_DT_FIFO = 1,
	VFS_DT_CHAR = 2,
	VFS_DT_DIR = 4,
	VFS_DT_BLOCK = 6,
	VFS_DT_REGULAR = 8,
	VFS_DT_SYMLINK = 10,
	VFS_DT_SOCKET = 12,
};

struct vfs_dentry;
struct vfs_file;
struct vfs_filesystem_type;
struct vfs_inode;
struct vfs_mount;
struct vfs_super_block;

struct vfs_timespec {
	int64 seconds;
	uint32 nanoseconds;
};

struct vfs_stat {
	uint64 dev;
	uint64 ino;
	enum vfs_inode_type type;
	uint32 mode;
	uint32 uid;
	uint32 gid;
	uint32 nlink;
	uint64 rdev;
	uint64 size;
	uint64 blocks;
	uint32 block_size;
	struct vfs_timespec atime;
	struct vfs_timespec mtime;
	struct vfs_timespec ctime;
};

struct vfs_statfs {
	uint64 type;
	uint64 block_size;
	uint64 blocks;
	uint64 blocks_free;
	uint64 blocks_available;
	uint64 files;
	uint64 files_free;
	uint64 name_length;
	uint64 fragment_size;
	uint64 flags;
};

struct vfs_iattr {
	uint32 mask;
	uint32 mode;
	uint32 uid;
	uint32 gid;
};

struct vfs_dirent {
	uint64 ino;
	uint64 next_offset;
	uint8 type;
	char name[VFS_NAME_MAX + 1];
};

typedef int (*vfs_dirent_emit_t)(const struct vfs_dirent *dirent,
				 void *context);

struct vfs_iovec {
	uint64 base;
	uint64 length;
};

struct vfs_pollfd {
	int fd;
	uint32 events;
	uint32 revents;
};

/*
 * Methods implemented by a filesystem.  VFS validates names and permissions
 * before calling them unless noted otherwise.  Returned inodes carry one VFS
 * reference.  Methods may sleep and must obey their filesystem's locking.
 */
struct vfs_inode_operations {
	int (*lookup)(struct vfs_inode *directory, const char *name,
	              struct vfs_inode **result);
	int (*create)(struct vfs_inode *directory, const char *name,
	              uint32 mode, uint32 uid, uint32 gid,
	              struct vfs_inode **result);
	int (*mkdir)(struct vfs_inode *directory, const char *name,
	             uint32 mode, uint32 uid, uint32 gid,
	             struct vfs_inode **result);
	int (*unlink)(struct vfs_inode *directory, const char *name);
	int (*rmdir)(struct vfs_inode *directory, const char *name);
	int (*rename)(struct vfs_inode *old_directory, const char *old_name,
	              struct vfs_inode *new_directory, const char *new_name,
	              uint32 flags);
	int (*link)(struct vfs_inode *inode, struct vfs_inode *directory,
	            const char *name);
	int (*symlink)(struct vfs_inode *directory, const char *name,
	               const char *target, uint32 uid, uint32 gid,
	               struct vfs_inode **result);
	int (*mknod)(struct vfs_inode *directory, const char *name,
	             enum vfs_inode_type type, uint32 mode, uint32 uid,
	             uint32 gid, uint64 device, struct vfs_inode **result);
	int (*readlink)(struct vfs_inode *inode, char *buffer, uint32 size);
	int (*truncate)(struct vfs_inode *inode, uint64 size);
	int (*setattr)(struct vfs_inode *inode,
	               const struct vfs_iattr *attributes);
	int (*set_times)(struct vfs_inode *inode,
	                 const struct vfs_timespec times[2],
	                 uint32 mask);
	int (*accessed)(struct vfs_inode *inode,
	               const struct vfs_timespec *time);
	int (*getattr)(struct vfs_inode *inode, struct vfs_stat *stat);
};

#define VFS_FILE_CAN_PREAD (1U << 0)

/*
 * Per-open file methods.  user_source/user_destination select user addresses;
 * the method must copy safely in that case.  Read/write return byte counts or
 * VFS errors and may return a short successful count.  @position is owned by
 * VFS or the caller according to the operation invoked.
 */
struct vfs_file_operations {
	uint32 flags;
	int (*open)(struct vfs_inode *inode, struct vfs_file *file);
	void (*release)(struct vfs_file *file);
	int64 (*read)(struct vfs_file *file, int user_destination,
	              uint64 destination, uint64 count, uint64 *position);
	int64 (*readv)(struct vfs_file *file, int user_destination,
		       const struct vfs_iovec *iovecs, uint32 count);
	int64 (*write)(struct vfs_file *file, int user_source, uint64 source,
	               uint64 count, uint64 *position);
	int64 (*writev)(struct vfs_file *file, int user_source,
		        const struct vfs_iovec *iovecs, uint32 count);
	int (*readdir)(struct vfs_file *file, struct vfs_dirent *dirent);
	int (*seekdir)(struct vfs_file *file, uint64 position);
	int64 (*ioctl)(struct vfs_file *file, uint64 request, uint64 argument);
	int (*fsync)(struct vfs_file *file);
	int (*fallocate)(struct vfs_file *file, uint64 offset, uint64 length);
	int (*getattr)(struct vfs_file *file, struct vfs_stat *stat);
	int (*set_flags)(struct vfs_file *file, uint32 flags);
	uint32 (*poll)(struct vfs_file *file, uint32 events);
};

/* Filesystem superblock teardown, sync, and capacity methods. */
struct vfs_super_operations {
	void (*put_inode)(struct vfs_inode *inode);
	int (*sync)(struct vfs_super_block *superblock);
	int (*statfs)(struct vfs_super_block *superblock,
	              struct vfs_statfs *stat);
	void (*unmount)(struct vfs_super_block *superblock);
};

/* Registered filesystem type.  mount() returns a referenced superblock. */
struct vfs_filesystem_type {
	const char *name;
	uint32 flags;
	int (*mount)(struct vfs_filesystem_type *type,
	             struct block_device *device, const void *data,
	             struct vfs_super_block **result);
};

#define VFS_FS_REQUIRES_DEVICE (1U << 0)
#define VFS_FS_NO_DENTRY_CACHE (1U << 1)
#define VFS_FS_SUPPORTS_ATIME  (1U << 2)

struct vfs_super_block {
	int ref;
	struct sleeplock write_lock;
	struct sleeplock attribute_lock;
	struct sleeplock atime_lock;
	struct vfs_filesystem_type *type;
	struct block_device *device;
	struct vfs_inode *root;
	const struct vfs_super_operations *operations;
	uint32 block_size;
	uint64 namespace_generation;
	void *private;
};

struct vfs_inode {
	int ref;
	uint32 write_open_count;
	uint32 exec_open_count;
	struct vfs_super_block *superblock;
	uint64 number;
	enum vfs_inode_type type;
	uint32 mode;
	uint32 uid;
	uint32 gid;
	uint32 nlink;
	uint64 device;
	uint64 size;
	uint64 blocks;
	struct vfs_timespec atime;
	struct vfs_timespec mtime;
	struct vfs_timespec ctime;
	const struct vfs_inode_operations *operations;
	const struct vfs_file_operations *file_operations;
	void *private;
};

struct vfs_dentry {
	int ref;
	int cached;
	struct vfs_dentry *parent;
	struct vfs_inode *inode;
	uint64 generation;
	uint64 last_used;
	char name[VFS_NAME_MAX + 1];
};

struct vfs_path {
	struct vfs_mount *mount;
	struct vfs_dentry *dentry;
};

struct vfs_mount {
	int ref;
	int attached;
	struct vfs_super_block *superblock;
	struct vfs_dentry *root;
	struct vfs_mount *parent;
	struct vfs_path mountpoint;
	uint32 flags;
};

struct vfs_mount_snapshot {
	char source[32];
	char target[VFS_PATH_MAX];
	char filesystem[32];
	uint32 flags;
};

struct vfs_file {
	int ref;
	uint32 access_ref;
	uint8 inode_access;
	struct sleeplock position_lock;
	struct vfs_path path;
	const struct vfs_file_operations *operations;
	uint32 capabilities;
	uint64 position;
	uint32 flags;
	void *private;
	void *device_private;
};

/**
 * vfs_init() - Initialize global namespace and descriptor state
 *
 * Context: Boot context, before filesystem registration or process creation.
 */
void vfs_init(void);

/**
 * vfs_register_filesystem() - Publish a filesystem type
 * @type: Static descriptor whose name and mount method remain valid forever.
 *
 * Context: Boot or serialized registration context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL, VFS_ERR_EXIST, or VFS_ERR_NOSPC.
 */
int vfs_register_filesystem(struct vfs_filesystem_type *type);

/**
 * vfs_super_alloc() - Allocate a VFS superblock owned by a filesystem
 * @type: Registered filesystem type.
 * @device: Optional backing device; VFS does not take a device reference here.
 *
 * Context: Thread context; may allocate memory.
 * Return: A zeroed superblock with one reference, or NULL on
 * allocation failure.
 */
struct vfs_super_block *vfs_super_alloc(struct vfs_filesystem_type *type,
					struct block_device *device);
void vfs_super_free(struct vfs_super_block *superblock);
/**
 * vfs_inode_alloc() - Allocate an inode wrapper for @superblock
 * @superblock: Live superblock that must outlive the inode; no ref is acquired.
 *
 * Context: Thread context; may allocate memory.
 * Return: An inode with one reference, or NULL on allocation failure.
 */
struct vfs_inode *vfs_inode_alloc(struct vfs_super_block *superblock);

/**
 * vfs_inode_get() - Acquire an inode reference
 * @inode: Non-NULL inode with a live reference retained by the caller.
 *
 * Context: Does not sleep.
 * Return: @inode with an additional reference. NULL or a dead inode panics.
 */
struct vfs_inode *vfs_inode_get(struct vfs_inode *inode);
void vfs_inode_put(struct vfs_inode *inode);
typedef int (*vfs_inode_visit_t)(struct vfs_inode *inode, void *context);
int vfs_visit_inodes(struct vfs_super_block *superblock,
		     vfs_inode_visit_t visit, void *context);
int vfs_inode_stat_default(struct vfs_inode *inode,
			   struct vfs_stat *stat);
int vfs_inode_stat(struct vfs_inode *inode, struct vfs_stat *stat);

/**
 * vfs_mount_root() - Mount the initial root filesystem
 * @filesystem: Registered filesystem type name.
 * @device_id: Backing block-device identifier, or zero when not required.
 * @flags: VFS_MOUNT_* flags.
 * @data: Filesystem-private immutable mount data, or NULL.
 *
 * Context: Serialized namespace context; may sleep in the mount method.
 * Return: VFS_OK or a VFS error.  It may be called only before a root exists.
 */
int vfs_mount_root(const char *filesystem, uint32 device_id,
		   uint32 flags, const void *data);
/**
 * vfs_mount() - Attach a filesystem at an existing directory
 * @filesystem: Registered filesystem type name.
 * @device_id: Backing block-device identifier, or zero when not required.
 * @target: Absolute or process-relative mountpoint path.
 * @flags: VFS_MOUNT_* flags.
 * @data: Filesystem-private immutable mount data, or NULL.
 *
 * Context: Thread context; may sleep and changes the shared namespace.
 * Return: VFS_OK or a VFS error; successful mounts remain until unmounted.
 */
int vfs_mount(const char *filesystem, uint32 device_id,
	      const char *target, uint32 flags, const void *data);
int vfs_mount_path(const char *filesystem, const char *source,
		   const char *target, uint32 flags, const void *data);
/**
 * vfs_unmount() - Detach a mounted filesystem
 * @target: Path resolving to the mount root.
 * @flags: Reserved unmount flags; currently must be zero.
 *
 * Context: Thread context; may sleep and fails while the mount is busy.
 * Return: VFS_OK or a VFS error.
 */
int vfs_unmount(const char *target, uint32 flags);
uint32 vfs_snapshot_mounts(struct vfs_mount_snapshot *snapshots,
			   uint32 capacity);
int vfs_get_root(struct vfs_path *path);
void vfs_path_copy(struct vfs_path *destination,
		   const struct vfs_path *source);
void vfs_path_put(struct vfs_path *path);

/**
 * vfs_open_file() - Resolve and open a path without installing an fd
 * @path: NUL-terminated process-relative or absolute path.
 * @flags: VFS_OPEN_* access and creation flags.
 * @mode: Requested permissions when @flags includes VFS_OPEN_CREATE.
 * @result: Receives a referenced open file on success.
 *
 * Context: Thread context; may sleep.  The caller owns *result and must put it.
 * Return: VFS_OK or a VFS path, permission, or backend error.
 */
int vfs_open_file(const char *path, uint32 flags, uint32 mode,
		  struct vfs_file **result);
/**
 * vfs_file_get() - Acquire a file reference
 * @file: Non-NULL file with a live reference retained by the caller.
 *
 * Context: Does not sleep.
 * Return: @file with an additional reference. NULL or a dead file panics.
 */
struct vfs_file *vfs_file_get(struct vfs_file *file);
void vfs_file_put(struct vfs_file *file);
struct vfs_file *vfs_file_hold(struct vfs_file *file);
void vfs_file_unhold(struct vfs_file *file);
void vfs_file_release_inode_access(struct vfs_file *file);
int vfs_exec_mapping_get(struct vfs_file *file);
void vfs_exec_mapping_put(struct vfs_file *file);
int vfs_file_mark_shared_dirty(struct vfs_file *file, uint64 offset);
void vfs_file_accessed(struct vfs_file *file);
/**
 * vfs_file_pread() - Read at an explicit offset
 * @file: Referenced open file.
 * @user_destination: Nonzero if @destination is a user virtual address.
 * @destination: Output address.
 * @count: Maximum byte count.
 * @offset: Byte offset; does not change file->position.
 *
 * Context: Thread context; may sleep.  The caller retains @file.
 * Return: Bytes read, possibly short, or a negative VFS error.
 */
int64 vfs_file_pread(struct vfs_file *file, int user_destination,
		     uint64 destination, uint64 count, uint64 offset);
int64 vfs_file_pread_raw(struct vfs_file *file, int user_destination,
			 uint64 destination, uint64 count, uint64 offset);
int64 vfs_file_pwrite_raw(struct vfs_file *file, int user_source,
			  uint64 source, uint64 count, uint64 offset);
/**
 * vfs_file_pwrite() - Write at an explicit offset
 * @file: Referenced open file.
 * @user_source: Nonzero if @source is a user virtual address.
 * @source: Input address.
 * @count: Maximum byte count.
 * @offset: Byte offset; does not change file->position.
 * @flags: VFS_WRITE_* modifiers.
 *
 * Context: Thread context; may sleep.  The caller retains @file.
 * Return: Bytes written, possibly short, or a negative VFS error.
 */
int64 vfs_file_pwrite(struct vfs_file *file, int user_source,
			 uint64 source, uint64 count, uint64 offset,
			 uint32 flags);
int64 vfs_file_preadv(struct vfs_file *file, int user_destination,
			 const struct vfs_iovec *iovecs, uint32 count,
			 uint64 offset);
int64 vfs_file_pwritev(struct vfs_file *file, int user_source,
			  const struct vfs_iovec *iovecs, uint32 count,
			  uint64 offset, uint32 flags);
int64 vfs_file_write_current(struct vfs_file *file, int user_source,
			     uint64 source, uint64 count);

/**
 * vfs_open() - Open a path and install it in the current process
 * descriptor table
 * @path: NUL-terminated process-relative or absolute path.
 * @flags: VFS_OPEN_* access and creation flags.
 * @mode: Requested permissions for creation.
 * @fd_out: Receives the new descriptor on success.
 *
 * Context: Current process context; may sleep.
 * Return: VFS_OK or a VFS error.  On success the process owns the descriptor.
 */
int vfs_open(const char *path, uint32 flags, uint32 mode, int *fd_out);
int vfs_install_file(struct vfs_file *file, uint8 flags, int *fd_out);
int vfs_get_file_fd(int fd, struct vfs_file **result);
uint32 vfs_file_poll(struct vfs_file *file, uint32 events);
int vfs_poll(struct vfs_pollfd *fds, uint32 count, int timeout_ms);
uint64 vfs_poll_generation(void);
int vfs_poll_wait(uint64 generation, int timeout_ms);
void vfs_poll_notify(void);
int vfs_close(int fd);
int vfs_dup(int oldfd, int minimum, uint8 flags, int *fd_out);
int vfs_dup_to(int oldfd, int newfd, uint8 flags);
int vfs_read(int fd, uint64 address, int length);
int vfs_write(int fd, uint64 address, int length);
int64 vfs_readv(int fd, int user_destination,
		const struct vfs_iovec *iovecs, uint32 count);
int64 vfs_writev(int fd, int user_source,
		 const struct vfs_iovec *iovecs, uint32 count,
		 uint32 flags);
int vfs_pipe(uint32 file_flags, uint8 fd_flags, int descriptors[2]);
int vfs_ftruncate(int fd, uint64 size);
int vfs_truncate(const char *path, uint64 size);
int vfs_fallocate(int fd, uint64 offset, uint64 length);
int64 vfs_ioctl(int fd, uint64 request, uint64 argument);
int vfs_seek(int fd, int64 offset, int whence, uint64 *result);
int vfs_stat_fd(int fd, struct vfs_stat *stat);
int vfs_stat_path(const char *path, int follow_symlink,
		  struct vfs_stat *stat);
int vfs_stat_at(int dirfd, const char *path, int follow_symlink,
		struct vfs_stat *stat);
int vfs_statfs_fd(int fd, struct vfs_statfs *stat);
int vfs_statfs_path(const char *path, struct vfs_statfs *stat);
int vfs_setattr_path(const char *path, int follow_symlink,
		     const struct vfs_iattr *attributes);
int vfs_setattr_at(int dirfd, const char *path, int follow_symlink,
		   const struct vfs_iattr *attributes);
int vfs_setattr_fd(int fd, const struct vfs_iattr *attributes);
int vfs_set_times_path(const char *path, int follow_symlink,
		       const struct vfs_timespec times[2], uint32 mask,
		       int owner_only);
int vfs_set_times_at(int dirfd, const char *path, int follow_symlink,
		     const struct vfs_timespec times[2], uint32 mask,
		     int owner_only);
int vfs_set_times_fd(int fd, const struct vfs_timespec times[2],
		     uint32 mask, int owner_only);
int vfs_current_time(struct vfs_timespec *time);
int vfs_next_dirent(struct vfs_file *file, vfs_dirent_emit_t emit,
		    void *context);
int vfs_mkdir(const char *path, uint32 mode);
int vfs_mknod(const char *path, enum vfs_inode_type type, uint32 mode,
	      uint64 device);
int vfs_mknod_at(int dirfd, const char *path, enum vfs_inode_type type,
		 uint32 mode, uint64 device);
int vfs_unlink(const char *path, int remove_directory);
int vfs_link(const char *old_path, const char *new_path,
	     int follow_symlink);
int vfs_symlink(const char *target, const char *link_path);
int vfs_readlink(const char *path, char *buffer, uint32 size);
int vfs_rename(const char *old_path, const char *new_path,
	       uint32 flags);
int vfs_fsync(int fd);
int vfs_sync(void);
int vfs_chdir(const char *path);
int vfs_getcwd(char *buffer, uint32 size);
int vfs_access(const char *path, uint32 mode, int use_effective_ids);
int vfs_get_fd_flags(int fd, uint8 *flags);
int vfs_set_fd_flags(int fd, uint8 flags);
int vfs_get_file_flags(int fd, uint32 *flags);
int vfs_set_file_flags(int fd, uint32 flags);
void vfs_close_on_exec(void);

#endif
