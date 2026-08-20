#include <block_device.h>
#include <debug.h>
#include <spinlock.h>

static struct {
	struct spinlock lock;
	struct block_device *devices[BLOCK_DEVICE_MAX];
} block_devices;

static void block_request_end_io_work(struct work_struct *work)
{
	struct block_request *request =
		container_of(work, struct block_request, end_io_work);
	block_end_io_t end_io;
	void *private;

	spinlock_acquire(&request->lock);
	if (!request->submitted || !request->completed ||
	    request->completion_done || !request->end_io)
		PANIC("invalid deferred block completion");
	end_io = request->end_io;
	private = request->private;
	spinlock_release(&request->lock);

	end_io(request, private);

	spinlock_acquire(&request->lock);
	if (request->completion_done)
		PANIC("duplicate deferred block completion");
	request->completion_done = 1;
	wait_queue_wake_all(&request->completion);
	spinlock_release(&request->lock);
}

void block_device_init(void)
{
	spinlock_init(&block_devices.lock, "block devices");
}

int block_device_register(struct block_device *device)
{
	uint32 id, first, last;

	if (!device || !device->name || !device->operations ||
	    !device->operations->submit ||
	    !device->sector_size || !device->sector_count ||
	    device->max_segments > BLOCK_REQUEST_MAX_SEGMENTS ||
	    device->sector_count > (uint64)-1 / device->sector_size)
		return -1;
	first = device->id ? device->id : 1;
	last = device->id ? device->id + 1 : BLOCK_DEVICE_MAX;
	if (first >= BLOCK_DEVICE_MAX)
		return -1;
	sleeplock_init(&device->raw_write_lock, "block raw write");
	wait_queue_init(&device->open_wait, "block device openers");
	device->open_count = 0;
	spinlock_acquire(&block_devices.lock);
	for (id = first; id < last; id++) {
		if (!block_devices.devices[id]) {
			device->id = id;
			block_devices.devices[id] = device;
			spinlock_release(&block_devices.lock);
			return 0;
		}
	}
	spinlock_release(&block_devices.lock);
	return -1;
}

void block_device_unregister(struct block_device *device)
{
	if (!device || !device->id || device->id >= BLOCK_DEVICE_MAX)
		return;
	spinlock_acquire(&block_devices.lock);
	if (block_devices.devices[device->id] != device) {
		spinlock_release(&block_devices.lock);
		return;
	}
	block_devices.devices[device->id] = 0;
	device->id = 0;
	while (device->open_count)
		wait_queue_sleep(&device->open_wait, &block_devices.lock);
	spinlock_release(&block_devices.lock);
}

struct block_device *block_device_get(uint32 id)
{
	struct block_device *device = 0;

	if (id >= BLOCK_DEVICE_MAX)
		return 0;
	spinlock_acquire(&block_devices.lock);
	device = block_devices.devices[id];
	spinlock_release(&block_devices.lock);
	return device;
}

struct block_device *block_device_open(uint32 id)
{
	struct block_device *device = 0;

	if (id >= BLOCK_DEVICE_MAX)
		return 0;
	spinlock_acquire(&block_devices.lock);
	device = block_devices.devices[id];
	if (device) {
		if (device->open_count == ~(uint32)0)
			device = 0;
		else
			device->open_count++;
	}
	spinlock_release(&block_devices.lock);
	return device;
}

void block_device_close(struct block_device *device)
{
	if (!device)
		return;
	spinlock_acquire(&block_devices.lock);
	if (!device->open_count)
		PANIC("block device close without open");
	device->open_count--;
	if (!device->open_count)
		wait_queue_wake_all(&device->open_wait);
	spinlock_release(&block_devices.lock);
}

static int block_device_range_valid(struct block_device *device,
				    uint64 sector, uint32 count)
{
	if (!device || !count || sector >= device->sector_count)
		return 0;
	return count <= device->sector_count - sector;
}

void block_request_init(struct block_request *request,
			struct block_device *device,
			enum block_request_operation operation,
			uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count)
{
	request->device = device;
	request->operation = operation;
	request->sector = sector;
	request->segments = segments;
	request->segment_count = segment_count;
	request->sector_count = 0;
	request->end_io = 0;
	request->private = 0;
	request->submitted = 0;
	request->completed = 0;
	request->completion_done = 0;
	request->status = -1;
	spinlock_init(&request->lock, "block request");
	wait_queue_init(&request->completion, "block completion");
	work_init(&request->end_io_work, block_request_end_io_work);
}

static int block_request_validate(struct block_request *request)
{
	uint64 count = 0;
	uint16 index;

	if (!request || !request->device ||
	    !request->device->operations ||
	    !request->device->operations->submit)
		return -1;
	if (request->operation == BLOCK_REQUEST_FLUSH)
		return request->segments || request->segment_count ? -1 : 0;
	if (request->operation != BLOCK_REQUEST_READ &&
	    request->operation != BLOCK_REQUEST_WRITE)
		return -1;
	if (!request->segments || !request->segment_count ||
	    request->segment_count > BLOCK_REQUEST_MAX_SEGMENTS ||
	    (request->device->max_segments &&
	     request->segment_count > request->device->max_segments))
		return -1;
	for (index = 0; index < request->segment_count; index++) {
		if (!request->segments[index].buffer ||
		    !request->segments[index].sector_count)
			return -1;
		count += request->segments[index].sector_count;
		if (count > 0xffffffffU)
			return -1;
	}
	request->sector_count = count;
	return block_device_range_valid(request->device, request->sector,
					request->sector_count) ? 0 : -1;
}

void block_request_complete(struct block_request *request, int status)
{
	int deferred;

	if (!request)
		PANIC("complete null block request");
	spinlock_acquire(&request->lock);
	if (!request->submitted || request->completed)
		PANIC("invalid block request completion");
	request->status = status;
	request->completed = 1;
	deferred = request->end_io != 0;
	if (!deferred) {
		request->completion_done = 1;
		wait_queue_wake_all(&request->completion);
	}
	spinlock_release(&request->lock);
	if (deferred && schedule_work(&request->end_io_work) != 1)
		PANIC("schedule deferred block completion");
}

int block_request_submit(struct block_request *request)
{
	int status;

	if (block_request_validate(request) < 0)
		return -1;
	spinlock_acquire(&request->lock);
	if (request->submitted) {
		spinlock_release(&request->lock);
		return -1;
	}
	request->submitted = 1;
	spinlock_release(&request->lock);
	status = request->device->operations->submit(request->device,
						      request);
	if (status < 0) {
		spinlock_acquire(&request->lock);
		if (!request->completed) {
			request->status = status;
			request->completed = 1;
			request->completion_done = 1;
			wait_queue_wake_all(&request->completion);
		}
		spinlock_release(&request->lock);
	}
	return status;
}

int block_request_wait(struct block_request *request)
{
	int deferred;
	int status;

	if (!request)
		return -1;
	spinlock_acquire(&request->lock);
	if (!request->submitted) {
		spinlock_release(&request->lock);
		return -1;
	}
	while (!request->completion_done)
		wait_queue_sleep(&request->completion, &request->lock);
	status = request->status;
	deferred = request->end_io != 0;
	spinlock_release(&request->lock);
	if (deferred)
		cancel_work_sync(&request->end_io_work);
	return status;
}

static int block_device_submit_wait(
	struct block_device *device, enum block_request_operation operation,
	uint64 sector, const struct block_segment *segments,
	uint16 segment_count)
{
	struct block_request request;

	block_request_init(&request, device, operation, sector, segments,
			   segment_count);
	if (block_request_submit(&request) < 0)
		return -1;
	return block_request_wait(&request);
}

int block_device_readv(struct block_device *device, uint64 sector,
		       const struct block_segment *segments,
		       uint16 segment_count)
{
	return block_device_submit_wait(device, BLOCK_REQUEST_READ, sector,
					segments, segment_count);
}

int block_device_writev(struct block_device *device, uint64 sector,
			const struct block_segment *segments,
			uint16 segment_count)
{
	return block_device_submit_wait(device, BLOCK_REQUEST_WRITE, sector,
					segments, segment_count);
}

int block_device_read(struct block_device *device, uint64 sector,
		      void *buffer, uint32 count)
{
	struct block_segment segment = {
		.buffer = buffer,
		.sector_count = count,
	};

	return block_device_readv(device, sector, &segment, 1);
}

int block_device_write(struct block_device *device, uint64 sector,
		       const void *buffer, uint32 count)
{
	struct block_segment segment = {
		.buffer = (void *)buffer,
		.sector_count = count,
	};

	return block_device_writev(device, sector, &segment, 1);
}

int block_device_flush(struct block_device *device)
{
	if (!device)
		return -1;
	return block_device_submit_wait(device, BLOCK_REQUEST_FLUSH, 0, 0, 0);
}
