/*
 * Character-device registration API.
 *
 * Drivers own char_device storage for the entire period between add and
 * remove.  devfs reads registered node metadata but does not own it.
 */
#ifndef __CAFFEINIX_KERNEL_CHAR_DEVICE_H
#define __CAFFEINIX_KERNEL_CHAR_DEVICE_H

#include <typedefs.h>
#include <vfs.h>

#define CHAR_DEVICE_NAME_MAX 31
#define CHAR_DEVICE_TERMINAL (1U << 0)
#define CHAR_DEVICE_CAN_PREAD (1U << 1)

struct char_device;

struct char_device_operations {
	int (*open)(struct char_device *device, struct vfs_file *file);
	void (*release)(struct char_device *device, struct vfs_file *file);
	int64 (*read)(struct char_device *device, struct vfs_file *file,
	              int user_destination, uint64 destination, uint64 count);
	int64 (*write)(struct char_device *device, struct vfs_file *file,
	               int user_source, uint64 source, uint64 count);
	int64 (*ioctl)(struct char_device *device, struct vfs_file *file,
	               uint64 request, uint64 argument);
	int (*fsync)(struct char_device *device, struct vfs_file *file);
	uint32 (*poll)(struct char_device *device, struct vfs_file *file,
		       uint32 events);
};

struct char_device {
	uint64 device;
	uint32 count;
	uint32 flags;
	const struct char_device_operations *operations;
	void *private;
	int registered;
};

struct char_device_node {
	char name[CHAR_DEVICE_NAME_MAX + 1];
	uint64 device;
	uint64 inode_number;
	uint32 mode;
};

/**
 * char_device_init() - Initialize the global character-device registry
 *
 * Context: Boot context, before drivers register devices.
 */
void char_device_init(void);

/**
 * char_device_region_register() - Reserve a major/minor number range
 * @device: First encoded VFS device number.
 * @count: Nonzero number of consecutive minor numbers.
 * @name: Stable diagnostic name; its storage must remain valid while reserved.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL for invalid arguments, VFS_ERR_BUSY for an
 * overlapping reservation, or VFS_ERR_NOSPC when the region table is full.
 */
int char_device_region_register(uint64 device, uint32 count,
				const char *name);

/**
 * char_device_region_unregister() - Release a previously reserved range
 * @device: First device number passed to registration.
 * @count: Exact registered range length.
 *
 * Context: No open users or registered devices may remain in the range.
 * Return: VFS_OK, VFS_ERR_BUSY while a registered driver overlaps the range,
 * or VFS_ERR_NOENT when no matching reservation exists.
 */
int char_device_region_unregister(uint64 device, uint32 count);

/**
 * char_device_add() - Publish a driver over an already reserved range
 * @device: Driver-owned descriptor, retained by the registry until removal.
 * @first: First encoded device number in the reserved range.
 * @count: Number of device numbers handled by @device.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK or a VFS error.  @device is unmodified on failure.
 */
int char_device_add(struct char_device *device, uint64 first,
		    uint32 count);
/**
 * char_device_remove() - Unpublish a character-device driver
 * @device: Registered driver to remove; its storage remains caller-owned.
 *
 * Context: Thread or boot context; does not sleep.  All nodes in its range
 * must have been removed first.
 * Return: VFS_OK, VFS_ERR_INVAL, VFS_ERR_BUSY, or VFS_ERR_NOENT.
 */
int char_device_remove(struct char_device *device);
/**
 * char_device_lookup() - Find a published character device
 * @device: Encoded major/minor device number.
 *
 * Context: Caller must prevent concurrent removal before dereferencing result.
 * Return: Matching driver descriptor, or NULL if none is registered.
 */
struct char_device *char_device_lookup(uint64 device);
/**
 * char_device_is_terminal() - Test terminal capability for a device number
 * @device: Encoded major/minor device number.
 *
 * Context: Does not sleep; removal must be excluded by the caller if the
 * result is used with a later dereference.
 * Return: Nonzero when a matching driver has CHAR_DEVICE_TERMINAL, else zero.
 */
int char_device_is_terminal(uint64 device);

/**
 * char_device_node_register() - Add a devfs-visible character-device name
 * @name: Nonempty NUL-terminated name no longer than CHAR_DEVICE_NAME_MAX.
 * @device: Registered encoded device number.
 * @mode: Mode bits recorded for the node.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL, VFS_ERR_EXIST, or VFS_ERR_NOSPC.
 */
int char_device_node_register(const char *name, uint64 device, uint32 mode);
/**
 * char_device_node_unregister() - Remove a devfs-visible node by name
 * @name: NUL-terminated registered name; NULL is invalid.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL, or VFS_ERR_NOENT.
 */
int char_device_node_unregister(const char *name);
/**
 * char_device_node_find() - Copy a node's metadata by name
 * @name: NUL-terminated registered name.
 * @node: Output metadata; must not be NULL.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL, or VFS_ERR_NOENT.
 */
int char_device_node_find(const char *name, struct char_device_node *node);
/**
 * char_device_node_get() - Copy metadata for the nth registered node
 * @index: Zero-based index among occupied node slots in slot order.
 * @node: Output metadata; must not be NULL.
 *
 * Context: Thread or boot context; does not sleep.
 * Return: VFS_OK, VFS_ERR_INVAL, or VFS_ERR_NOENT.
 */
int char_device_node_get(uint32 index, struct char_device_node *node);
/**
 * char_device_node_count() - Count registered devfs-visible nodes
 *
 * Context: Thread or boot context; does not sleep.
 * Return: Number of occupied node slots.
 */
uint32 char_device_node_count(void);

extern const struct vfs_file_operations vfs_device_operations;

#endif
