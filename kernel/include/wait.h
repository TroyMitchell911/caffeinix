#ifndef __CAFFEINIX_KERNEL_WAIT_H
#define __CAFFEINIX_KERNEL_WAIT_H

#include <list.h>
#include <spinlock.h>

struct thread;

#define WAIT_QUEUE_TIMEOUT     -1
#define WAIT_QUEUE_INTERRUPTED -2
#define WAIT_QUEUE_TERMINATED  -3

typedef struct wait_queue {
	struct spinlock lock;
	struct list waiters;
	const char *name;
} *wait_queue_t;

/*
 * The owner must prevent new sleepers and verify that the queue is empty
 * before reclaiming or reinitializing the storage containing it.
 */
void wait_queue_init(wait_queue_t queue, const char *name);
void wait_queue_sleep(wait_queue_t queue, spinlock_t condition_lock);
int wait_queue_sleep_timeout(wait_queue_t queue,
			     spinlock_t condition_lock, uint64 timeout_ms);
int wait_queue_sleep_interruptible(wait_queue_t queue,
				   spinlock_t condition_lock);
int wait_queue_sleep_killable(wait_queue_t queue,
			      spinlock_t condition_lock);
int wait_queue_sleep_interruptible_timeout(wait_queue_t queue,
					   spinlock_t condition_lock,
					   uint64 timeout_ms);
int wait_queue_sleep_interruptible_until(wait_queue_t queue,
					 spinlock_t condition_lock,
					 uint64 deadline);
int wait_queue_wake_one(wait_queue_t queue);
int wait_queue_wake_all(wait_queue_t queue);
int wait_queue_wake_mask(wait_queue_t queue, int count, uint32 mask);
int wait_queue_requeue(wait_queue_t source, wait_queue_t destination,
		       int count, void *wait_private);
int wait_queue_wake_thread(struct thread *thread);
int wait_queue_signal_thread(struct thread *thread, int fatal);
int wait_queue_terminate_thread(struct thread *thread);
int wait_queue_empty(wait_queue_t queue);
void wait_queue_timeout_init(void);
void wait_queue_expire(uint64 now);

#endif
