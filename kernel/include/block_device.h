/*
 * Asynchronous block-device request interface.
 *
 * A submitted request remains caller-owned until completion.  Drivers must
 * complete it exactly once; synchronous helpers wait in process context.
 */
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

/**
 * block_device_init() - Initialize the global block-device registry.
 *
 * Context:
 * Early boot before device registration; does not sleep.
 */
void block_device_init(void);
/**
 * block_device_register() - Publish a hardware block device.
 * @device: Caller-owned device with geometry and submit operation.
 *
 * Context:
 * Process context; initializes its open and raw-write synchronization.
 * Return:
 * Zero on success, negative for invalid geometry or no free ID.
 */
int block_device_register(struct block_device *device);
/**
 * block_device_unregister() - Remove a block device after all opens close.
 * @device: Registered device.
 *
 * Context:
 * Process context; may sleep waiting for open_count to reach zero.
 */
void block_device_unregister(struct block_device *device);
/**
 * block_device_get() - Look up a registered device without opening it.
 * @id: Registry ID.
 *
 * The pointer is borrowed; this does not take an open reference. The caller
 * must exclude concurrent unregister while using it, or use
 * block_device_open() to acquire a reference by ID instead.
 *
 * Context:
 * Any context; does not sleep.
 * Return:
 * Device or %NULL.
 */
struct block_device *block_device_get(uint32 id);
/**
 * block_device_open() - Acquire one open reference to a block device.
 * @id: Registry ID.
 *
 * Context:
 * Any context; does not sleep.  Pair a non-NULL result with close.
 * Return:
 * Open device or %NULL.
 */
struct block_device *block_device_open(uint32 id);
/**
 * block_device_close() - Release an open reference.
 * @device: Device returned by block_device_open().
 *
 * Context:
 * Any context; wakes unregistration after the final close.
 */
void block_device_close(struct block_device *device);
/**
 * block_request_init() - Initialize a caller-owned request before submission.
 * @request: Storage that remains valid through completion and callback.
 * @device: Registered target device.
 * @operation: Read, write, or flush operation.
 * @sector: First device sector.
 * @segments: Read/write scatterlist, or %NULL for flush.
 * @segment_count: Number of @segments.
 *
 * Context:
 * Process context before submit; no allocation or locking required.
 */
void block_request_init(struct block_request *request,
			struct block_device *device,
			enum block_request_operation operation,
			uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count);
/**
 * block_request_submit() - Validate and hand a request to its driver.
 * @request: Initialized request not already submitted.
 *
 * Context:
 * Process context; driver may queue asynchronously but must not free
 * @request.  Do not alter segments until complete.
 * Return:
 * Zero on successful submission or negative validation/driver error.
 */
int block_request_submit(struct block_request *request);
/**
 * block_request_wait() - Sleep until a submitted request completes.
 * @request: Successfully submitted request.
 *
 * Context:
 * Process context; may sleep.  Completion callback, if set, finishes
 * before this returns.
 * Return:
 * Driver completion status.
 */
int block_request_wait(struct block_request *request);
/**
 * block_request_complete() - Complete one driver-owned in-flight request.
 * @request: Submitted request.
 * @status: Zero or driver error status.
 *
 * Context:
 * IRQ-safe; invokes end_io later in process context when present.
 */
void block_request_complete(struct block_request *request, int status);
/**
 * block_device_readv() - Synchronously read sector segments.
 * @device: Open or otherwise stable device.
 * @sector: First sector.
 * @segments: Non-empty destination scatterlist.
 * @segment_count: Number of segments.
 *
 * Context:
 * Process context; may sleep.
 * Return:
 * Zero or a negative submission/completion error.
 */
int block_device_readv(struct block_device *device, uint64 sector,
		       const struct block_segment *segments,
		       uint16 segment_count);
/**
 * block_device_writev() - Synchronously write sector segments.
 * @device: Open or otherwise stable device.
 * @sector: First sector.
 * @segments: Non-empty source scatterlist.
 * @segment_count: Number of segments.
 *
 * Context:
 * Process context; may sleep.
 * Return:
 * Zero or a negative submission/completion error.
 */
int block_device_writev(struct block_device *device, uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count);
/**
 * block_device_read() - Read consecutive sectors into one buffer.
 * @device: Open or stable device.
 * @sector: First sector.
 * @buffer: Non-NULL destination buffer.
 * @count: Non-zero sector count.
 *
 * Context:
 * Process context; may sleep.
 * Return:
 * Zero or negative error.
 */
int block_device_read(struct block_device *device, uint64 sector,
		      void *buffer, uint32 count);
/**
 * block_device_write() - Write consecutive sectors from one buffer.
 * @device: Open or stable device.
 * @sector: First sector.
 * @buffer: Non-NULL source buffer.
 * @count: Non-zero sector count.
 *
 * Context:
 * Process context; may sleep.
 * Return:
 * Zero or negative error.
 */
int block_device_write(struct block_device *device, uint64 sector,
		       const void *buffer, uint32 count);
/**
 * block_device_flush() - Persist prior writes where the driver supports it.
 * @device: Open or stable device.
 *
 * Context:
 * Process context; may sleep.
 * Return:
 * Zero or negative error.
 */
int block_device_flush(struct block_device *device);
int block_core_selftest_start(void);
int virtio_blk_init(void);
void virtio_blk_debug_dump(void);

extern const struct vfs_file_operations vfs_block_device_operations;

#endif
