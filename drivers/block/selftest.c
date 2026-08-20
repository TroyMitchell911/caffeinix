#include <block_device.h>
#include <debug.h>
#include <scheduler.h>
#include <thread.h>

struct block_test_context {
	struct block_device *device;
	uint32 id;
	int close_finished;
};

static int block_test_read(struct block_device *device, uint64 sector,
			   void *buffer, uint32 count)
{
	(void)device;
	(void)sector;
	(void)buffer;
	(void)count;
	return 0;
}

static int block_test_write(struct block_device *device, uint64 sector,
			    const void *buffer, uint32 count)
{
	(void)device;
	(void)sector;
	(void)buffer;
	(void)count;
	return 0;
}

static const struct block_device_operations block_test_operations = {
	.read = block_test_read,
	.write = block_test_write,
};

static void block_test_close(void *argument)
{
	struct block_test_context *context = argument;

	while (block_device_get(context->id) == context->device)
		yield();
	block_device_close(context->device);
	__atomic_store_n(&context->close_finished, 1, __ATOMIC_RELEASE);
}

static void block_test_run(void *argument)
{
	struct block_device device = {
		.name = "block-selftest",
		.id = BLOCK_DEVICE_MAX - 1,
		.sector_size = 512,
		.sector_count = 1,
		.operations = &block_test_operations,
	};
	struct block_device *opened;
	struct block_test_context context = {
		.device = &device,
	};
	thread_t closer;
	uint32 id;

	(void)argument;
	if (block_device_register(&device))
		PANIC("block selftest register");
	id = device.id;
	context.id = id;
	opened = block_device_open(id);
	if (opened != &device || device.open_count != 1) {
		if (opened)
			block_device_close(opened);
		block_device_unregister(&device);
		PANIC("block selftest open");
	}
	closer = kernel_thread_create("block-close", block_test_close,
				      &context);
	if (!closer) {
		block_device_close(&device);
		block_device_unregister(&device);
		PANIC("block selftest closer");
	}
	block_device_unregister(&device);
	while (!__atomic_load_n(&context.close_finished, __ATOMIC_ACQUIRE))
		yield();
	if (device.id || device.open_count || block_device_get(id) ||
	    block_device_open(id))
		PANIC("block selftest lifetime");
}

int block_core_selftest_start(void)
{
	return kernel_thread_create("block-test", block_test_run, 0) ? 0 : -1;
}
