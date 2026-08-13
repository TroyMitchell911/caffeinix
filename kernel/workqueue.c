#include <debug.h>
#include <kernel_config.h>
#include <scheduler.h>
#include <spinlock.h>
#include <wait.h>
#include <workqueue.h>

static struct {
	struct spinlock lock;
	struct wait_queue wait;
	struct list pending;
	struct work_struct *current;
	thread_t worker;
} system_workqueue;

static void workqueue_thread(void *argument)
{
	(void)argument;

	for (;;) {
		struct work_struct *work;
		struct list *node;

		spinlock_acquire(&system_workqueue.lock);
		while (system_workqueue.pending.next ==
		       &system_workqueue.pending)
			wait_queue_sleep(&system_workqueue.wait,
			                 &system_workqueue.lock);
		node = system_workqueue.pending.next;
		list_remove(node);
		work = list_entry(node, struct work_struct, node);
		work->pending = 0;
		work->running = 1;
		system_workqueue.current = work;
		spinlock_release(&system_workqueue.lock);
		work->function(work);
		spinlock_acquire(&system_workqueue.lock);
		system_workqueue.current = 0;
		work->running = 0;
		wait_queue_wake_all(&work->completion);
		spinlock_release(&system_workqueue.lock);
	}
}

void workqueue_init(void)
{
	spinlock_init(&system_workqueue.lock, "system workqueue");
	wait_queue_init(&system_workqueue.wait, "system workqueue");
	list_init(&system_workqueue.pending);
	system_workqueue.current = 0;
	system_workqueue.worker =
		kernel_thread_create(WORKQUEUE_NAME, workqueue_thread, 0);
	if (!system_workqueue.worker)
		PANIC("create system workqueue");
}

void work_init(struct work_struct *work, work_func_t function)
{
	if (!work || !function)
		PANIC("invalid work");
	list_init(&work->node);
	work->function = function;
	work->pending = 0;
	work->running = 0;
	wait_queue_init(&work->completion, "work completion");
}

int schedule_work(struct work_struct *work)
{
	if (!work || !work->function)
		return -1;
	spinlock_acquire(&system_workqueue.lock);
	if (work->pending) {
		spinlock_release(&system_workqueue.lock);
		return 0;
	}
	work->pending = 1;
	list_insert_before(&system_workqueue.pending, &work->node);
	wait_queue_wake_one(&system_workqueue.wait);
	spinlock_release(&system_workqueue.lock);
	return 1;
}

int cancel_work(struct work_struct *work)
{
	if (!work)
		return -1;
	spinlock_acquire(&system_workqueue.lock);
	if (!work->pending) {
		spinlock_release(&system_workqueue.lock);
		return 0;
	}
	list_remove(&work->node);
	work->pending = 0;
	spinlock_release(&system_workqueue.lock);
	return 1;
}

int cancel_work_sync(struct work_struct *work)
{
	int cancelled = 0;

	if (!work)
		return -1;
	spinlock_acquire(&system_workqueue.lock);
	for (;;) {
		if (work->pending) {
			list_remove(&work->node);
			work->pending = 0;
			cancelled = 1;
		}
		if (!work->running)
			break;
		cancelled = 1;
		if (system_workqueue.current == work &&
		    cur_thread() == system_workqueue.worker)
			break;
		wait_queue_sleep(&work->completion,
				 &system_workqueue.lock);
	}
	spinlock_release(&system_workqueue.lock);
	return cancelled;
}

int workqueue_in_worker(void)
{
	return system_workqueue.worker &&
	       cur_thread() == system_workqueue.worker;
}
