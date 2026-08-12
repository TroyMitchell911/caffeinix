#ifndef __CAFFEINIX_KERNEL_WAIT_H
#define __CAFFEINIX_KERNEL_WAIT_H

#include <list.h>
#include <spinlock.h>

struct thread;

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
int wait_queue_wake_one(wait_queue_t queue);
int wait_queue_wake_all(wait_queue_t queue);
int wait_queue_wake_thread(struct thread *thread);
int wait_queue_empty(wait_queue_t queue);

#endif
