#ifndef __CAFFEINIX_KERNEL_CHAR_DEVICE_H
#define __CAFFEINIX_KERNEL_CHAR_DEVICE_H

#include <typedefs.h>
#include <vfs.h>

#define CHAR_DEVICE_NAME_MAX 31
#define CHAR_DEVICE_TERMINAL (1U << 0)

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

void char_device_init(void);
int char_device_region_register(uint64 device, uint32 count,
				const char *name);
int char_device_region_unregister(uint64 device, uint32 count);
int char_device_add(struct char_device *device, uint64 first,
		    uint32 count);
int char_device_remove(struct char_device *device);
struct char_device *char_device_lookup(uint64 device);
int char_device_is_terminal(uint64 device);

int char_device_node_register(const char *name, uint64 device, uint32 mode);
int char_device_node_unregister(const char *name);
int char_device_node_find(const char *name, struct char_device_node *node);
int char_device_node_get(uint32 index, struct char_device_node *node);
uint32 char_device_node_count(void);

extern const struct vfs_file_operations vfs_device_operations;

#endif
