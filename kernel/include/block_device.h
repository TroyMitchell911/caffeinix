#ifndef __CAFFEINIX_KERNEL_BLOCK_DEVICE_H
#define __CAFFEINIX_KERNEL_BLOCK_DEVICE_H

#include <typedefs.h>

#define BLOCK_DEVICE_MAX 8

struct block_device;

struct block_device_operations {
	int (*read)(struct block_device *device, uint64 sector,
	            void *buffer, uint32 count);
	int (*write)(struct block_device *device, uint64 sector,
	             const void *buffer, uint32 count);
	int (*flush)(struct block_device *device);
};

struct block_device {
	const char *name;
	uint32 id;
	uint32 sector_size;
	uint64 sector_count;
	const struct block_device_operations *operations;
	void *private;
};

void block_device_init(void);
int block_device_register(struct block_device *device);
void block_device_unregister(struct block_device *device);
struct block_device *block_device_get(uint32 id);
int block_device_read(struct block_device *device, uint64 sector,
		      void *buffer, uint32 count);
int block_device_write(struct block_device *device, uint64 sector,
		       const void *buffer, uint32 count);
int block_device_flush(struct block_device *device);
int virtio_blk_init(void);
void virtio_blk_debug_dump(void);

#endif
