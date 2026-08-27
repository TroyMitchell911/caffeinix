#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <block_device.h>

static struct block_request *pending;
static struct work_struct *pending_work;
static unsigned int callbacks;
static unsigned int completion_wakes;
static unsigned int submissions;
static unsigned int work_syncs;
static int autocomplete;
static int defer_work_retirement;
static spinlock_t completion_condition_lock;
static wait_queue_t monitored_completion;

struct completion_context {
	struct block_request *request;
	struct block_device *device;
	unsigned char buffer[512];
	int followup_status;
};

void panic(char *message)
{
	fprintf(stderr, "block test panic: %s\n", message);
	exit(EXIT_FAILURE);
}

void spinlock_init(spinlock_t lock, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->name = name;
}

void spinlock_acquire(spinlock_t lock)
{
	if (lock->locked)
		panic("recursive test lock");
	lock->locked = 1;
}

void spinlock_release(spinlock_t lock)
{
	if (!lock->locked)
		panic("unheld test lock");
	lock->locked = 0;
}

void sleeplock_init(sleeplock_t lock, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->name = name;
}

void wait_queue_init(wait_queue_t queue, const char *name)
{
	spinlock_init(&queue->lock, name);
	list_init(&queue->waiters);
	queue->name = name;
}

void wait_queue_sleep(wait_queue_t queue, spinlock_t condition_lock)
{
	(void)queue;
	(void)condition_lock;
	panic("unexpected test sleep");
}

int wait_queue_wake_all(wait_queue_t queue)
{
	if (queue == monitored_completion) {
		if (!completion_condition_lock->locked)
			panic("completion wake without condition lock");
		if (!callbacks)
			panic("completion wake before callback");
		completion_wakes++;
	}
	return 0;
}

void work_init(struct work_struct *work, work_func_t function)
{
	list_init(&work->node);
	work->function = function;
	work->pending = 0;
	work->running = 0;
	wait_queue_init(&work->completion, "test work completion");
}

int schedule_work(struct work_struct *work)
{
	if (!work || !work->function)
		return -1;
	if (work->pending)
		return 0;
	if (pending_work)
		panic("multiple pending test work items");
	work->pending = 1;
	pending_work = work;
	return 1;
}

int cancel_work_sync(struct work_struct *work)
{
	if (!work)
		return -1;
	if (work->pending)
		panic("synchronize pending test work");
	if (!work->running)
		panic("synchronize retired test work");
	work->running = 0;
	work_syncs++;
	return 1;
}

static void run_pending_work(void)
{
	struct work_struct *work = pending_work;

	if (!work)
		panic("missing pending test work");
	pending_work = NULL;
	work->pending = 0;
	work->running = 1;
	work->function(work);
	if (!defer_work_retirement)
		work->running = 0;
}

static int fake_submit(struct block_device *device,
		       struct block_request *request)
{
	(void)device;
	if (pending)
		return -1;
	pending = request;
	submissions++;
	if (autocomplete) {
		pending = NULL;
		block_request_complete(request, 0);
	}
	return 0;
}

static const struct block_device_operations fake_operations = {
	.submit = fake_submit,
};

static void completed(struct block_request *request, void *private)
{
	struct completion_context *context = private;

	if (request != context->request)
		panic("completion private data");
	callbacks++;
	context->followup_status =
		block_device_read(context->device, 0, context->buffer, 1);
}

static int test_async(struct block_device *device)
{
	unsigned char buffers[3][1024];
	struct block_segment segments[] = {
		{ buffers[0], 2 },
		{ buffers[1], 1 },
		{ buffers[2], 2 },
	};
	struct block_request request;
	struct completion_context context = {
		.request = &request,
		.device = device,
		.followup_status = -1,
	};

	autocomplete = 0;
	block_request_init(&request, device, BLOCK_REQUEST_READ, 7,
			   segments, 3);
	request.end_io = completed;
	request.private = &context;
	if (block_request_submit(&request) || pending != &request ||
	    request.sector_count != 5 || request.completed ||
	    submissions != 1)
		return -1;
	pending = NULL;
	completion_condition_lock = &request.lock;
	monitored_completion = &request.completion;
	block_request_complete(&request, -7);
	if (!request.completed || request.completion_done || callbacks ||
	    completion_wakes || pending_work != &request.end_io_work)
		return -1;
	autocomplete = 1;
	defer_work_retirement = 1;
	run_pending_work();
	completion_condition_lock = NULL;
	monitored_completion = NULL;
	if (!request.completed || !request.completion_done ||
	    callbacks != 1 || completion_wakes != 1 ||
	    context.followup_status || pending_work || pending || work_syncs ||
	    !request.end_io_work.running)
		return -1;
	if (block_request_wait(&request) != -7 || work_syncs != 1 ||
	    request.end_io_work.running)
		return -1;
	defer_work_retirement = 0;
	if (!block_request_submit(&request))
		return -1;
	return 0;
}

static int test_merged_io(struct block_device *device)
{
	unsigned char buffers[3][1024];
	struct block_segment segments[] = {
		{ buffers[0], 2 },
		{ buffers[1], 2 },
		{ buffers[2], 2 },
	};
	unsigned int before = submissions;

	autocomplete = 1;
	if (block_device_readv(device, 9, segments, 3) ||
	    submissions != before + 1 || pending)
		return -1;
	if (block_device_write(device, 20, buffers[0], 2) ||
	    block_device_flush(device) || submissions != before + 3)
		return -1;
	return 0;
}

static int test_validation(struct block_device *device)
{
	unsigned char buffers[3][512];
	struct block_segment segments[3] = {
		{ buffers[0], 1 },
		{ buffers[1], 1 },
		{ buffers[2], 1 },
	};
	struct block_request request;

	block_request_init(&request, device, BLOCK_REQUEST_READ,
			   device->sector_count, segments, 1);
	if (!block_request_submit(&request))
		return -1;
	block_request_init(&request, device, BLOCK_REQUEST_FLUSH, 0,
			   segments, 1);
	if (!block_request_submit(&request))
		return -1;
	segments[0].buffer = NULL;
	block_request_init(&request, device, BLOCK_REQUEST_WRITE, 0,
			   segments, 1);
	if (!block_request_submit(&request))
		return -1;
	segments[0].buffer = buffers[0];
	device->max_segments = 2;
	block_request_init(&request, device, BLOCK_REQUEST_READ, 0,
			   segments, 3);
	if (!block_request_submit(&request) || pending)
		return -1;
	device->max_segments = BLOCK_REQUEST_MAX_SEGMENTS + 1;
	if (!block_device_register(device))
		return -1;
	device->max_segments = 0;
	return 0;
}

int main(void)
{
	struct block_device device = {
		.name = "fake0",
		.sector_size = 512,
		.sector_count = 64,
		.operations = &fake_operations,
	};

	block_device_init();
	if (block_device_register(&device) ||
	    block_device_get(device.id) != &device ||
	    test_async(&device) || test_merged_io(&device) ||
	    test_validation(&device)) {
		fputs("block validation failed\n", stderr);
		return EXIT_FAILURE;
	}
	block_device_unregister(&device);
	if (device.id || block_device_get(1))
		return EXIT_FAILURE;
	puts("BLOCK_OK");
	return EXIT_SUCCESS;
}
