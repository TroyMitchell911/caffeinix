# CFS-style scheduler

Caffeinix uses a small SMP scheduler based on the classic Linux Completely
Fair Scheduler model. It is an independent implementation of the same core
ideas, not copied Linux scheduler source and not a claim of full Linux
scheduler compatibility.

## Model

Every runnable thread has a scheduling entity containing virtual runtime,
actual runtime, current-slice start, nice value, weight, and red-black-tree
node. One global runqueue orders entities by virtual runtime and uses the
thread ID as a stable tie breaker.

Runtime is charged when a running thread yields, blocks, exits, changes nice,
or is examined for preemption. Virtual runtime advances as:

```text
delta_vruntime = delta_execution * NICE_0_LOAD / weight
```

The 40-entry weight table for nice values -20 through 19 matches the classic
Linux CFS weights. A lower-priority task therefore advances farther in
virtual time for the same physical execution and is selected less often.

The leftmost tree entity has the least virtual runtime and runs next. A new
thread begins at the runqueue minimum. A waking thread that fell too far
behind is placed no earlier than one wakeup granularity behind that minimum,
preventing unlimited sleeper credit.

## Slices and preemption

The target latency is 48 ms, minimum granularity is 6 ms, wakeup granularity
is 4 ms, and the absolute minimum slice is 1 ms. A selected entity receives
its weighted share of the scheduling period. The timer requests reschedule
when the slice expires or when the current entity is sufficiently to the
right of the leftmost runnable entity. The actual context switch happens at
a trap-return safe point.

A waking entity can request preemption of the CPU with the greatest virtual
runtime. Idle CPUs are preferred and are awakened through the SBI IPI
extension. The implementation coalesces reschedule requests.

## SMP state and locking

`runqueue.lock` protects the tree, aggregate weight, runnable count, minimum
virtual runtime, and each CPU's scheduler-visible `current` and `selected`
state. `selected` covers the interval after a CPU removes an entity from the
tree but before it obtains that thread's lock and starts it. This prevents
the entity from disappearing from load and minimum-runtime accounting.

A thread lock is held across every context switch back to the scheduler.
The scheduler verifies that a thread is in exactly one of these locations:
running on one CPU, selected by one CPU, queued once, sleeping on one wait
queue, exited, or unused.

Process exit holds the final thread lock while publishing zombie state and
until the scheduler switches away. A parent on another CPU can observe the
zombie immediately, but cannot reclaim its thread or kernel stack before the
handoff completes.

## Thread lifecycle

Thread-table entries progress from `THREAD_UNUSED` to `THREAD_ALLOCATED`,
then to `THREAD_RUNNABLE` once the scheduler queues them. A selected thread is
temporarily absent from the tree but remains accounted as `cpu->selected`;
after the context switch it is `THREAD_RUNNING`. Blocking changes a running
thread to `THREAD_SLEEPING`, and a wakeup requeues it. Exit changes it to
`THREAD_EXITED`. Reclamation starts only after the old context has switched
back to the scheduler stack: the scheduler directly reaps kernel threads and
non-final user threads. The final user thread remains process-owned while its
group is a zombie, and process reaping later releases its thread storage.

The thread lock protects an entry throughout these transitions. A user thread
also belongs to a process: paths that need both locks acquire the process lock
before the thread lock. Reaping follows that order even when it begins with a
thread lock, by dropping and reacquiring the thread lock before taking the
process lock. This prevents an exiting thread from being freed while another
CPU can still switch through its kernel stack.

Kernel threads have negative TIDs and no owning process or trapframe. Their
entry trampoline releases the thread lock, enables interrupts, calls the
registered function, and exits through the scheduler. User-thread allocation
creates a trapframe page and records the entry in its process slot before the
thread can become runnable. Allocation failure leaves no partially published
entry.

## Accounting and observability

Runtime is charged at scheduler transitions and on kernel/user mode changes.
`scheduler_thread_times()` includes time elapsed since the last charge for a
currently running thread, so process accounting need not force a context
switch. The aggregate idle-time and context-switch counters are lockless
observability snapshots; they are monotonic measurements, not synchronization
primitives.

`scheduler_set_nice()` accepts only Linux nice values -20 through 19. It
charges a running task before changing its weight, removes and reinserts a
queued entity to preserve tree ordering, and requests rescheduling if needed.
There is no affinity API: all runnable work remains eligible for every online
CPU.

## Idle policy

An empty CPU publishes itself idle while synchronized with enqueue, then
executes `WFI` with interrupt sources enabled. Logical CPU 0 retains the
configured 1 ms tick even while idle so it can expire global timed waits.
Other idle CPUs use the configured 100 ms idle interval; returning to active
execution restores their 1 ms tick. Runnable work wakes an idle CPU with an
IPI instead of waiting for either timer.

## Current limits

There is one global runqueue rather than Linux per-CPU runqueues. Caffeinix
does not yet implement CPU affinity, load balancing, scheduler classes,
real-time policies, cgroups, bandwidth control, NUMA placement, or CPU
hotplug. Modern Linux also has scheduler evolution beyond the classic CFS
picker; this milestone intentionally implements the smaller vruntime model
needed by Caffeinix.

The host benchmark under `tests/scripts/benchmark-scheduler.py` compares one
and eight guest CPUs, records command medians, and measures idle QEMU CPU
use. Guest tests cover nice-weight ordering, more runnable processes than
CPUs, fork/exec/exit/wait, blocking I/O, timed waits, and mixed CPU, network,
and filesystem pressure.

To reproduce the runtime coverage locally, run:

```sh
make -C tests qemu
```

The test harness boots CPU-count and memory-size matrices, runs scheduler
runtime tests in the guest, and fails on panics, hangs, or accounting errors.
