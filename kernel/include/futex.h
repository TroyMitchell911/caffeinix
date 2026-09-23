/* Linux RISC-V futex and robust-list support. */
#ifndef __CAFFEINIX_KERNEL_FUTEX_H
#define __CAFFEINIX_KERNEL_FUTEX_H
#include <thread.h>

/**
 * futex_init() - Initialize the fixed-size futex key table
 *
 * Context: Single-threaded initialization before futex syscalls are exposed.
 */
void futex_init(void);

/**
 * futex_thread_exit() - Release robust futex state for an exiting thread
 * @thread: Thread whose user address space remains valid during cleanup.
 *
 * Context: Thread-exit path. Wakes one waiter for each recovered robust lock
 * and clears clear_child_tid when that user mapping is writable.
 */
void futex_thread_exit(thread_t thread);

/**
 * futex_restart_cancel() - Discard saved restart state
 * @thread: Thread whose futex restart state is to be cleared; may be NULL.
 *
 * Context: Signal or syscall handling with exclusive access to @thread.
 */
void futex_restart_cancel(thread_t thread);

/**
 * futex_restart_signal() - Select restart behavior after signal delivery
 * @thread: Thread with a potentially interrupted futex wait; may be NULL.
 * @through_handler: Nonzero when execution will enter a signal handler.
 *
 * Context: Signal handling with exclusive access to @thread.
 */
void futex_restart_signal(thread_t thread, int through_handler);

/**
 * futex_restart_sigreturn() - Arm a saved futex wait after sigreturn
 * @thread: Returning thread; may be NULL.
 *
 * Context: sigreturn handling with exclusive access to @thread. State is
 * discarded when the restored syscall register state no longer matches.
 */
void futex_restart_sigreturn(thread_t thread);

#endif
