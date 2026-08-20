#ifndef __CAFFEINIX_KERNEL_BLOCK_DEVICE_H
#define __CAFFEINIX_KERNEL_BLOCK_DEVICE_H

#include <sleeplock.h>
#include <spinlock.h>
#include <typedefs.h>
#include <wait.h>
#include <workqueue.h>

#define BLOCK_DEVICE_MAX 8
#define BLOCK_DEVICE_NODE_MAJOR 252
#define BLOCK_REQUEST_MAX_SEGMENTS 30

struct block_device;
struct block_request;
struct vfs_file_operations;

enum block_request_operation {
	BLOCK_REQUEST_READ,
	BLOCK_REQUEST_WRITE,
	BLOCK_REQUEST_FLUSH,
};

struct block_segment {
	void *buffer;
	uint32 sector_count;
};

typedef void (*block_end_io_t)(struct block_request *request, void *private);

struct block_request {
	struct block_device *device;
	enum block_request_operation operation;
	uint64 sector;
	const struct block_segment *segments;
	uint16 segment_count;
	uint32 sector_count;
	struct spinlock lock;
	struct wait_queue completion;
	struct work_struct end_io_work;
	block_end_io_t end_io;
	void *private;
	uint8 submitted;
	uint8 completed;
	uint8 completion_done;
	int status;
};

struct block_device_operations {
	int (*submit)(struct block_device *device,
	              struct block_request *request);
};

struct block_device {
	const char *name;
	uint32 id;
	uint32 sector_size;
	uint64 sector_count;
	uint32 open_count;
	uint16 max_segments;
	struct sleeplock raw_write_lock;
	struct wait_queue open_wait;
	const struct block_device_operations *operations;
	void *private;
};

void block_device_init(void);
int block_device_register(struct block_device *device);
void block_device_unregister(struct block_device *device);
struct block_device *block_device_get(uint32 id);
struct block_device *block_device_open(uint32 id);
void block_device_close(struct block_device *device);
void block_request_init(struct block_request *request,
			struct block_device *device,
			enum block_request_operation operation,
			uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count);
int block_request_submit(struct block_request *request);
int block_request_wait(struct block_request *request);
void block_request_complete(struct block_request *request, int status);
int block_device_readv(struct block_device *device, uint64 sector,
		       const struct block_segment *segments,
		       uint16 segment_count);
int block_device_writev(struct block_device *device, uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count);
int block_device_read(struct block_device *device, uint64 sector,
		      void *buffer, uint32 count);
int block_device_write(struct block_device *device, uint64 sector,
		       const void *buffer, uint32 count);
int block_device_flush(struct block_device *device);
int block_core_selftest_start(void);
int virtio_blk_init(void);
void virtio_blk_debug_dump(void);

extern const struct vfs_file_operations vfs_block_device_operations;

#endif
