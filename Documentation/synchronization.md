# Synchronization primitives

This document describes Caffeinix's kernel synchronization primitives. They
coordinate kernel threads and interrupt-facing code; they are not userspace
ABI objects. Public calling contracts live in the corresponding headers.

## Spin locks

`spinlock_t` protects short critical sections shared by CPUs or interrupt
contexts. Acquisition disables local interrupts, and the outermost matching
release restores their prior state. This prevents an interrupt handler from
waiting for a lock held by code it interrupted on the same CPU.

Spin locks cannot be held across any operation that may sleep, including wait
queue sleeps and sleep-lock acquisition. The lock implementation supplies the
acquire and release ordering for data protected by the lock. Ownership checks
are diagnostic only; they do not make an unlocked access safe.

## Wait queues

A wait queue represents an event, not a condition. The caller owns the
condition and protects it with a separate spin lock. The required pattern is:

1. Acquire the condition lock and test the condition.
2. While it is false, sleep on the wait queue with that lock.
3. Test the condition again after every return.

The sleep operation links the thread before releasing the condition lock and
reacquires it before it returns. This prevents the check-to-sleep gap from
losing a wakeup. A wakeup only makes a thread runnable; it does not guarantee
that the condition is now true, because another thread can consume the event
first.

Timed waits are ordered by a global deadline queue. Interruptible waits also
record the signal sequence while preparing to sleep, closing the race between
the initial pending-signal check and waiter publication. They return a wait
status rather than delivering a signal themselves; the syscall or subsystem
maps that status to its own error and restart rules.

The internal lock order is global timeout queue, wait queue, then thread.
Users must keep wait queue storage alive until they have excluded new sleepers
and verified the queue is empty.

## Sleep locks

A sleep lock serializes thread-context operations that may block. Its embedded
spin lock protects the owner and waiter list. A contending thread sleeps only
after it is linked to the waiter list, so release cannot miss it. Sleep locks
are non-recursive and must not be acquired from interrupt context or while a
spin lock is held. Releasing one wakes a single waiter; every waiter loops and
rechecks ownership.

## Workqueue

The system workqueue has one kernel worker and defers callbacks to thread
context. `schedule_work()` coalesces repeated submissions while an item is
pending. The callback runs without the queue lock, after which its completion
wait queue wakes synchronous cancellers.

The caller owns `struct work_struct` storage. It must remain valid from
`work_init()` until it is neither pending nor running. Before reclaiming the
object, call `cancel_work_sync()` from a different thread. A callback must not
use `cancel_work_sync()` on itself to authorize reclamation: the implementation
detects self-cancellation and returns without waiting for the callback to end,
so the work storage is still running and must remain valid.

## Futexes and robust lists

Linux futex keys identify a 32-bit userspace word. Private keys use the
process and virtual address. Shared keys resolve the mapped physical address
so mappings of the same word share waiters. The fixed slot table bounds the
number of simultaneously active keys; slot references track attached waiters.

FUTEX_WAIT verifies the user word while holding the table lock, then attaches
the caller before releasing that lock for the wait. FUTEX_WAKE and requeue use
the same lock, so a matching wake cannot pass between verification and waiter
publication. Waiting is interruptible and maps a signal to the syscall restart
path. Relative timeout restart state preserves the original deadline rather
than extending the requested interval after signal handling.

On thread exit, robust-list processing walks a bounded user list, marks locks
owned by the exiting TID with `OWNER_DIED`, and wakes one private and shared
waiter for each recovered address. Malformed, inaccessible, or excessively
long user lists stop only their affected cleanup; they never authorize kernel
memory access.

## Membarrier

The Linux `membarrier` subset supports query, private-expedited registration,
and private-expedited barriers. Registration is tracked per process. A
private-expedited request requires prior registration and delegates the actual
cross-CPU memory ordering to `cpu_membarrier()`.

## Validation

Run the host checks and QEMU runtime suite:

```sh
make -C tests qemu
```

The QEMU suite covers futex waits, wakeups, requeueing, timeout and signal
restart paths, plus workqueue and scheduler interactions exercised by kernel
runtime tests. Use the project's normal toolchain and rootfs prerequisites as
described in the top-level README.
