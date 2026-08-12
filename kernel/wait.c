#include <debug.h>
#include <scheduler.h>
#include <thread.h>
#include <wait.h>

void wait_queue_init(wait_queue_t queue, const char *name)
{
	spinlock_init(&queue->lock, name);
	list_init(&queue->waiters);
	queue->name = name;
}

void wait_queue_sleep(wait_queue_t queue, spinlock_t condition_lock)
{
	thread_t current = cur_thread();

	if (!current || !spinlock_holding(condition_lock))
		PANIC("wait without condition lock");
	spinlock_acquire(&queue->lock);
	spinlock_acquire(&current->lock);
	if (current->state != THREAD_RUNNING || current->on_waitqueue ||
	    current->waiting_on)
		PANIC("invalid wait state");
	current->waiting_on = queue;
	current->on_waitqueue = 1;
	list_insert_before(&queue->waiters, &current->wait_node);
	current->state = THREAD_SLEEPING;
	spinlock_release(condition_lock);
	spinlock_release(&queue->lock);

	sched();

	if (current->on_waitqueue || current->waiting_on)
		PANIC("scheduled wait entry");
	spinlock_release(&current->lock);
	spinlock_acquire(condition_lock);
}

static void wait_queue_wake_locked(wait_queue_t queue, thread_t thread)
{
	spinlock_acquire(&thread->lock);
	if (!thread->on_waitqueue || thread->waiting_on != queue ||
	    thread->state != THREAD_SLEEPING)
		PANIC("invalid wait queue entry");
	list_remove(&thread->wait_node);
	thread->on_waitqueue = 0;
	thread->waiting_on = 0;
	scheduler_make_runnable(thread);
	spinlock_release(&thread->lock);
}

int wait_queue_wake_one(wait_queue_t queue)
{
	thread_t thread;
	list_t node;

	spinlock_acquire(&queue->lock);
	node = queue->waiters.next;
	if (node == &queue->waiters) {
		spinlock_release(&queue->lock);
		return 0;
	}
	thread = list_entry(node, struct thread, wait_node);
	wait_queue_wake_locked(queue, thread);
	spinlock_release(&queue->lock);
	return 1;
}

int wait_queue_wake_all(wait_queue_t queue)
{
	thread_t thread;
	list_t node;
	int count = 0;

	spinlock_acquire(&queue->lock);
	while ((node = queue->waiters.next) != &queue->waiters) {
		thread = list_entry(node, struct thread, wait_node);
		wait_queue_wake_locked(queue, thread);
		count++;
	}
	spinlock_release(&queue->lock);
	return count;
}

int wait_queue_wake_thread(thread_t thread)
{
	wait_queue_t queue;

	for (;;) {
		spinlock_acquire(&thread->lock);
		if (thread->state != THREAD_SLEEPING ||
		    !thread->on_waitqueue || !thread->waiting_on) {
			spinlock_release(&thread->lock);
			return 0;
		}
		queue = thread->waiting_on;
		/*
		 * Normal wakeup takes queue->lock before thread->lock. Do not
		 * block in the inverse order: retry so that an in-flight waker
		 * can remove the entry. Keeping thread->lock across a successful
		 * trylock also keeps the attached queue alive.
		 */
		if (spinlock_trylock(&queue->lock)) {
			spinlock_release(&thread->lock);
			continue;
		}
		list_remove(&thread->wait_node);
		thread->on_waitqueue = 0;
		thread->waiting_on = 0;
		scheduler_make_runnable(thread);
		spinlock_release(&queue->lock);
		spinlock_release(&thread->lock);
		return 1;
	}
}

int wait_queue_empty(wait_queue_t queue)
{
	int empty;

	spinlock_acquire(&queue->lock);
	empty = queue->waiters.next == &queue->waiters;
	spinlock_release(&queue->lock);
	return empty;
}
