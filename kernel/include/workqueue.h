/*
 * The system workqueue defers process-context work to one kernel worker.
 * A work item may be pending or running, but callers retain its storage.
 */
#ifndef __CAFFEINIX_KERNEL_WORKQUEUE_H
#define __CAFFEINIX_KERNEL_WORKQUEUE_H
#include <list.h>
#include <typedefs.h>
#include <wait.h>

struct work_struct;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	struct list node;
	work_func_t function;
	uint8 pending;
	uint8 running;
	struct wait_queue completion;
};

/**
 * workqueue_init() - Start the system workqueue worker
 *
 * Context: Initialization context, once before work_init() or scheduling.
 * May create a kernel thread. The workqueue remains available thereafter.
 */
void workqueue_init(void);

/**
 * work_init() - Initialize a reusable work item
 * @work: Caller-owned storage, not pending or running.
 * @function: Non-NULL callback invoked by the worker in thread context.
 *
 * Context: Externally serialized context. @work must remain live until any
 * scheduled execution has completed or cancel_work_sync() has returned.
 */
void work_init(struct work_struct *work, work_func_t function);

/**
 * schedule_work() - Queue work for asynchronous execution
 * @work: Initialized work item whose storage remains live.
 *
 * Context: Any context that can take the system workqueue spin lock. Does
 * not sleep.
 * Return: 1 when queued, 0 when already pending, or -1 on error.
 */
int schedule_work(struct work_struct *work);

/**
 * cancel_work() - Remove queued but not running work
 * @work: Initialized work item.
 *
 * Context: Any non-sleeping context. Does not wait for an executing callback.
 * Return: 1 when removed, 0 when not pending, or -1 for a NULL item.
 */
int cancel_work(struct work_struct *work);

/**
 * cancel_work_sync() - Cancel work and wait for a running callback
 * @work: Initialized work item.
 *
 * Context: Thread context; may sleep. It must not be called from @work's own
 * callback. Return: 1 if work was pending or running, 0 if idle, or -1 when
 * @work is NULL.
 */
int cancel_work_sync(struct work_struct *work);

/**
 * workqueue_in_worker() - Test whether the caller is the system worker
 *
 * Context: Any context.
 * Return: Nonzero only in the system worker thread.
 */
int workqueue_in_worker(void);

#endif
