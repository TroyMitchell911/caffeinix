/*
 * Sleep locks serialize thread-context work that can block. They are not
 * recursive and cannot be acquired from interrupt context.
 */
#ifndef __CAFFEINIX_KERNEL_SLEEP_LOCK_H
#define __CAFFEINIX_KERNEL_SLEEP_LOCK_H
#include <spinlock.h>
#include <wait.h>

struct thread;

typedef struct sleeplock{
        uint8 locked;
        struct spinlock lk;
        struct wait_queue waiters;

        const char* name;
        struct thread *owner;
}*sleeplock_t;

/**
 * sleeplock_init() - Initialize a sleep lock
 * @lk: Caller-owned lock storage that is not concurrently accessible.
 * @name: Stable diagnostic name, or NULL when no name is available.
 *
 * Context: Early boot or externally serialized thread context. Does not
 * sleep. The storage must outlive all lock users and waiters.
 */
void sleeplock_init(sleeplock_t lk, const char *name);

/**
 * sleeplock_holding() - Test whether the current thread owns a lock
 * @lk: Initialized sleep lock.
 *
 * Context: Thread context.
 * Return: Nonzero when the current thread owns it.
 */
uint8 sleeplock_holding(sleeplock_t lk);

/**
 * sleeplock_acquire() - Acquire a sleep lock, waiting if necessary
 * @lk: Initialized sleep lock not held by the current thread.
 *
 * Context: Thread context; may sleep. The caller must release @lk exactly
 * once and must not hold spin locks across this call.
 */
void sleeplock_acquire(sleeplock_t lk);

/**
 * sleeplock_release() - Release a sleep lock held by the current thread
 * @lk: Initialized sleep lock owned by the current thread.
 *
 * Context: Thread context. Wakes one waiter after dropping ownership.
 */
void sleeplock_release(sleeplock_t lk);

#endif
