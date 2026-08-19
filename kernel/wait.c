#include <debug.h>
#include <ktime.h>
#include <process.h>
#include <scheduler.h>
#include <thread.h>
#include <wait.h>

static struct {
	struct spinlock lock;
	struct list waiters;
} timeout_queue;

void wait_queue_timeout_init(void)
{
	spinlock_init(&timeout_queue.lock, "wait timeouts");
	list_init(&timeout_queue.waiters);
}

void wait_queue_init(wait_queue_t queue, const char *name)
{
	spinlock_init(&queue->lock, name);
	list_init(&queue->waiters);
	queue->name = name;
}

static void timeout_insert_locked(thread_t thread)
{
	struct list *node;

	for (node = timeout_queue.waiters.next;
	     node != &timeout_queue.waiters; node = node->next) {
		thread_t queued;

		queued = list_entry(node, struct thread, timeout_node);
		if (thread->wait_deadline < queued->wait_deadline)
			break;
	}
	list_insert_before(node, &thread->timeout_node);
	thread->on_timeout_queue = 1;
}

static int wait_queue_sleep_deadline(wait_queue_t queue,
				     spinlock_t condition_lock,
				     uint64 deadline)
{
	thread_t current = cur_thread();
	int exit_status, result;

	if (!current || !spinlock_holding(condition_lock))
		PANIC("wait without condition lock");
	if (process_thread_exit_requested(current, &exit_status)) {
		spinlock_release(condition_lock);
		process_thread_exit(exit_status, 0);
		PANIC("terminated wait returned");
	}
	spinlock_acquire(&timeout_queue.lock);
	spinlock_acquire(&queue->lock);
	spinlock_acquire(&current->lock);
	if (current->state != THREAD_RUNNING || current->on_waitqueue ||
	    current->waiting_on || current->on_timeout_queue)
		PANIC("invalid wait state");
	current->waiting_on = queue;
	current->on_waitqueue = 1;
	current->wait_deadline = deadline;
	current->wait_result = 0;
	list_insert_before(&queue->waiters, &current->wait_node);
	if (deadline)
		timeout_insert_locked(current);
	if (process_thread_exit_requested(current, &exit_status)) {
		list_remove(&current->wait_node);
		current->on_waitqueue = 0;
		current->waiting_on = 0;
		if (current->on_timeout_queue) {
			list_remove(&current->timeout_node);
			current->on_timeout_queue = 0;
		}
		current->wait_result = WAIT_QUEUE_TERMINATED;
		spinlock_release(&current->lock);
		spinlock_release(&queue->lock);
		spinlock_release(&timeout_queue.lock);
		spinlock_release(condition_lock);
		process_thread_exit(exit_status, 0);
		PANIC("terminated wait returned");
	}
	scheduler_block_current();
	spinlock_release(condition_lock);
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);

	sched();

	if (current->on_waitqueue || current->waiting_on ||
	    current->on_timeout_queue)
		PANIC("scheduled wait entry");
	result = current->wait_result;
	spinlock_release(&current->lock);
	spinlock_acquire(condition_lock);
	if (result == WAIT_QUEUE_TERMINATED) {
		if (!process_thread_exit_requested(current, &exit_status))
			PANIC("terminated wait without request");
		spinlock_release(condition_lock);
		process_thread_exit(exit_status, 0);
		PANIC("terminated wait returned");
	}
	return result;
}

void wait_queue_sleep(wait_queue_t queue, spinlock_t condition_lock)
{
	(void)wait_queue_sleep_deadline(queue, condition_lock, 0);
}

int wait_queue_sleep_timeout(wait_queue_t queue,
			     spinlock_t condition_lock, uint64 timeout_ms)
{
	uint64 now, delta, deadline;

	if (!timeout_ms)
		return -1;
	now = ktime_get_ticks();
	delta = ktime_ms_to_ticks(timeout_ms);
	deadline = now + delta;
	if (deadline < now)
		deadline = ~(uint64)0;
	return wait_queue_sleep_deadline(queue, condition_lock, deadline);
}

static void wait_queue_wake_locked(wait_queue_t queue, thread_t thread,
				   int result)
{
	spinlock_acquire(&thread->lock);
	if (!thread->on_waitqueue || thread->waiting_on != queue ||
	    thread->state != THREAD_SLEEPING)
		PANIC("invalid wait queue entry");
	list_remove(&thread->wait_node);
	thread->on_waitqueue = 0;
	thread->waiting_on = 0;
	if (thread->on_timeout_queue) {
		list_remove(&thread->timeout_node);
		thread->on_timeout_queue = 0;
	}
	thread->wait_result = result;
	scheduler_make_runnable(thread);
	spinlock_release(&thread->lock);
}

int wait_queue_wake_one(wait_queue_t queue)
{
	thread_t thread;
	list_t node;

	spinlock_acquire(&timeout_queue.lock);
	spinlock_acquire(&queue->lock);
	node = queue->waiters.next;
	if (node == &queue->waiters) {
		spinlock_release(&queue->lock);
		spinlock_release(&timeout_queue.lock);
		return 0;
	}
	thread = list_entry(node, struct thread, wait_node);
	wait_queue_wake_locked(queue, thread, 0);
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);
	return 1;
}

int wait_queue_wake_all(wait_queue_t queue)
{
	thread_t thread;
	list_t node;
	int count = 0;

	spinlock_acquire(&timeout_queue.lock);
	spinlock_acquire(&queue->lock);
	while ((node = queue->waiters.next) != &queue->waiters) {
		thread = list_entry(node, struct thread, wait_node);
		wait_queue_wake_locked(queue, thread, 0);
		count++;
	}
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);
	return count;
}

int wait_queue_wake_mask(wait_queue_t queue, int count, uint32 mask)
{
	thread_t thread;
	list_t node, next;
	int woken = 0;

	if (count <= 0 || !mask)
		return 0;
	spinlock_acquire(&timeout_queue.lock);
	spinlock_acquire(&queue->lock);
	for (node = queue->waiters.next;
	     node != &queue->waiters && woken < count; node = next) {
		next = node->next;
		thread = list_entry(node, struct thread, wait_node);
		if (!(thread->wait_bitset & mask))
			continue;
		wait_queue_wake_locked(queue, thread, 0);
		woken++;
	}
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);
	return woken;
}

int wait_queue_requeue(wait_queue_t source, wait_queue_t destination,
		       int count, void *wait_private)
{
	thread_t thread;
	list_t node;
	int moved = 0;

	if (source == destination || count <= 0)
		return 0;
	spinlock_acquire(&timeout_queue.lock);
	spinlock_acquire(&source->lock);
	spinlock_acquire(&destination->lock);
	while (moved < count &&
	       (node = source->waiters.next) != &source->waiters) {
		thread = list_entry(node, struct thread, wait_node);
		spinlock_acquire(&thread->lock);
		if (thread->state != THREAD_SLEEPING ||
		    !thread->on_waitqueue || thread->waiting_on != source)
			PANIC("invalid requeue entry");
		list_remove(&thread->wait_node);
		list_insert_before(&destination->waiters,
		                   &thread->wait_node);
		thread->waiting_on = destination;
		thread->wait_private = wait_private;
		spinlock_release(&thread->lock);
		moved++;
	}
	spinlock_release(&destination->lock);
	spinlock_release(&source->lock);
	spinlock_release(&timeout_queue.lock);
	return moved;
}

int wait_queue_wake_thread(thread_t thread)
{
	wait_queue_t queue;

	spinlock_acquire(&timeout_queue.lock);
	queue = thread->waiting_on;
	if (!queue) {
		spinlock_release(&timeout_queue.lock);
		return 0;
	}
	spinlock_acquire(&queue->lock);
	spinlock_acquire(&thread->lock);
	if (thread->state != THREAD_SLEEPING || !thread->on_waitqueue ||
	    thread->waiting_on != queue) {
		spinlock_release(&thread->lock);
		spinlock_release(&queue->lock);
		spinlock_release(&timeout_queue.lock);
		return 0;
	}
	list_remove(&thread->wait_node);
	thread->on_waitqueue = 0;
	thread->waiting_on = 0;
	if (thread->on_timeout_queue) {
		list_remove(&thread->timeout_node);
		thread->on_timeout_queue = 0;
	}
	thread->wait_result = 0;
	scheduler_make_runnable(thread);
	spinlock_release(&thread->lock);
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);
	return 1;
}

int wait_queue_terminate_thread(thread_t thread)
{
	wait_queue_t queue;

	spinlock_acquire(&timeout_queue.lock);
	queue = thread->waiting_on;
	if (!queue) {
		spinlock_release(&timeout_queue.lock);
		return 0;
	}
	spinlock_acquire(&queue->lock);
	if (thread->state != THREAD_SLEEPING || !thread->on_waitqueue ||
	    thread->waiting_on != queue) {
		spinlock_release(&queue->lock);
		spinlock_release(&timeout_queue.lock);
		return 0;
	}
	wait_queue_wake_locked(queue, thread, WAIT_QUEUE_TERMINATED);
	spinlock_release(&queue->lock);
	spinlock_release(&timeout_queue.lock);
	return 1;
}

void wait_queue_expire(uint64 now)
{
	struct list *node;
	thread_t thread;
	wait_queue_t queue;

	spinlock_acquire(&timeout_queue.lock);
	while ((node = timeout_queue.waiters.next) !=
	       &timeout_queue.waiters) {
		thread = list_entry(node, struct thread, timeout_node);
		if (thread->wait_deadline > now)
			break;
		queue = thread->waiting_on;
		if (!queue)
			PANIC("timeout without wait queue");
		spinlock_acquire(&queue->lock);
		wait_queue_wake_locked(queue, thread, -1);
		spinlock_release(&queue->lock);
	}
	spinlock_release(&timeout_queue.lock);
}

int wait_queue_empty(wait_queue_t queue)
{
	int empty;

	spinlock_acquire(&queue->lock);
	empty = queue->waiters.next == &queue->waiters;
	spinlock_release(&queue->lock);
	return empty;
}
