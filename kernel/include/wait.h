/*
 * Wait queues block threads until an externally protected condition changes.
 * A waiter is linked before dropping the caller's condition lock, preventing
 * lost wakeups. Every sleeper must recheck its condition after wakeup.
 */
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

/**
 * wait_queue_init() - Initialize queue storage before publication
 * @queue: Caller-owned wait queue storage.
 * @name: Stable diagnostic name, or NULL when no name is available.
 *
 * Context: Early boot or externally serialized context. Queue storage must
 * remain live while users can sleep on it; reclaim requires no new waiters
 * and an empty queue.
 */
void wait_queue_init(wait_queue_t queue, const char *name);

/**
 * wait_queue_sleep() - Sleep until a queue wakeup
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the caller's wait condition.
 *
 * Context: Thread context; may sleep. Atomically links the caller before
 * releasing @condition_lock and reacquires it before returning. Callers must
 * use a loop to recheck their condition.
 */
void wait_queue_sleep(wait_queue_t queue, spinlock_t condition_lock);

/**
 * wait_queue_sleep_timeout() - Sleep until wakeup or timeout
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the wait condition.
 * @timeout_ms: Relative timeout in milliseconds; zero does not sleep.
 *
 * Context: Thread context; may sleep. @condition_lock is held on return.
 * Return: 0 after wakeup, %WAIT_QUEUE_TIMEOUT after expiration, or -1 when
 * @timeout_ms is zero.
 */
int wait_queue_sleep_timeout(wait_queue_t queue,
			     spinlock_t condition_lock, uint64 timeout_ms);

/**
 * wait_queue_sleep_interruptible() - Sleep until wakeup or unblocked signal
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the wait condition.
 *
 * Context: Thread context; may sleep. @condition_lock is held on return.
 * Return: 0 after wakeup or %WAIT_QUEUE_INTERRUPTED for a pending signal.
 */
int wait_queue_sleep_interruptible(wait_queue_t queue,
				   spinlock_t condition_lock);

/**
 * wait_queue_sleep_killable() - Sleep until wakeup or fatal signal
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the wait condition.
 *
 * Context: Thread context; may sleep. @condition_lock is held on return.
 * Return: 0 after wakeup or %WAIT_QUEUE_INTERRUPTED for a fatal signal.
 */
int wait_queue_sleep_killable(wait_queue_t queue,
			      spinlock_t condition_lock);

/**
 * wait_queue_sleep_interruptible_timeout() - Interruptible relative sleep
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the wait condition.
 * @timeout_ms: Relative timeout in milliseconds; zero does not sleep.
 *
 * Context: Thread context; may sleep. @condition_lock is held on return.
 * Return: 0, %WAIT_QUEUE_TIMEOUT, or %WAIT_QUEUE_INTERRUPTED.
 */
int wait_queue_sleep_interruptible_timeout(wait_queue_t queue,
					   spinlock_t condition_lock,
					   uint64 timeout_ms);

/**
 * wait_queue_sleep_interruptible_until() - Interruptible absolute sleep
 * @queue: Initialized queue whose storage remains live.
 * @condition_lock: Held spin lock protecting the wait condition.
 * @deadline: Absolute scheduler tick at which the wait expires.
 *
 * Context: Thread context; may sleep. @condition_lock is held on return.
 * Return: 0, %WAIT_QUEUE_TIMEOUT, or %WAIT_QUEUE_INTERRUPTED.
 */
int wait_queue_sleep_interruptible_until(wait_queue_t queue,
					 spinlock_t condition_lock,
					 uint64 deadline);

/**
 * wait_queue_wake_one() - Wake one waiter
 * @queue: Initialized queue.
 *
 * Context: Any non-sleeping context.
 * Return: 1 when a thread was woken, otherwise 0. A wakeup does not itself
 * make a caller's condition true.
 */
int wait_queue_wake_one(wait_queue_t queue);

/**
 * wait_queue_wake_all() - Wake all current waiters
 * @queue: Initialized queue.
 *
 * Context: Any non-sleeping context.
 * Return: Number of threads woken.
 */
int wait_queue_wake_all(wait_queue_t queue);

/**
 * wait_queue_wake_mask() - Wake compatible waiters up to a limit
 * @queue: Initialized queue.
 * @count: Positive maximum number of waiters to wake.
 * @mask: Nonzero bit mask intersected with each waiter's bitset.
 *
 * Context: Any non-sleeping context.
 * Return: Number of threads woken.
 */
int wait_queue_wake_mask(wait_queue_t queue, int count, uint32 mask);

/**
 * wait_queue_requeue() - Move waiters without waking them
 * @source: Initialized source queue.
 * @destination: Distinct initialized destination queue.
 * @count: Positive maximum number of waiters to move.
 * @wait_private: Value stored in each moved thread's wait-private field.
 *
 * Context: Any non-sleeping context.
 * Return: Number of threads moved.
 */
int wait_queue_requeue(wait_queue_t source, wait_queue_t destination,
		       int count, void *wait_private);

/**
 * wait_queue_wake_thread() - Wake a specific queued thread
 * @thread: Thread that may be sleeping on a wait queue.
 *
 * Context: Any non-sleeping context.
 * Return: 1 when woken, otherwise 0.
 */
int wait_queue_wake_thread(struct thread *thread);

/**
 * wait_queue_signal_thread() - Interrupt a signal-responsive wait
 * @thread: Thread that may be sleeping on a wait queue.
 * @fatal: Nonzero for a fatal signal; zero for a nonfatal signal.
 *
 * Context: Signal delivery context.
 * Return: 1 when an eligible waiter was woken with
 * %WAIT_QUEUE_INTERRUPTED, otherwise 0.
 */
int wait_queue_signal_thread(struct thread *thread, int fatal);

/**
 * wait_queue_terminate_thread() - End a blocked thread's wait for exit
 * @thread: Thread that may be sleeping on a wait queue.
 *
 * Context: Thread-exit coordination.
 * Return: 1 when a waiter was woken with %WAIT_QUEUE_TERMINATED, otherwise
 * 0.
 */
int wait_queue_terminate_thread(struct thread *thread);

/**
 * wait_queue_empty() - Test whether a queue has no linked waiters
 * @queue: Initialized queue.
 *
 * Context: Any non-sleeping context.
 * Return: Nonzero when empty. This is a snapshot and cannot authorize
 * reclamation without external exclusion.
 */
int wait_queue_empty(wait_queue_t queue);

/**
 * wait_queue_timeout_init() - Initialize the global timeout ordering queue
 *
 * Context: Single-threaded initialization before timed waits are used.
 */
void wait_queue_timeout_init(void);

/**
 * wait_queue_expire() - Wake all waiters whose deadline has elapsed
 * @now: Current scheduler tick value.
 *
 * Context: Timer or scheduler context. Does not sleep.
 */
void wait_queue_expire(uint64 now);

#endif
