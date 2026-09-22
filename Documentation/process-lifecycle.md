# Process lifecycle

The process subsystem owns the unit exposed by Linux process, credential,
signal, job-control, and wait system calls. A process is a thread group: it
owns an address space, open-file table, VMA collection, credentials, signal
dispositions, and a bounded set of threads. The thread-group leader has the
same PID and TID.

## States and ownership

Allocation creates an `EMBRYO` object with its process lock held. Once its
first thread is runnable, it becomes `LIVE`. A terminating group becomes a
`ZOMBIE`; its exit status, accounting, and parent relation remain observable
until a parent waits or auto-reaping destroys it. Final destruction removes an
object from the global list and releases residual object state and thread
storage. The last exiting thread releases user mappings, VMA backing, file
descriptors, and root/cwd path references before zombie publication.

The global process-list/wait lock serializes lookup, reparenting, and child
events. For published processes, code taking both it and a process lock takes
the global lock first. Process allocation is the deliberate exception: its
private EMBRYO object is locked before insertion into the global list. The
process lock protects group state and signal actions; file and VMA state have
their own locks. A process may not be freed while a child, thread, signal,
sleep, or vfork wait queue still has a waiter.

## Creation and replacement

`userinit()` creates the sole initial task. Its first scheduled entry mounts
the configured root filesystem, devfs, tmpfs, and procfs, configures console
standard I/O, and executes `INIT_PATH` (currently `/bin/sh`). Mounting is
boot-only; ordinary process starts never remount the root.

`fork` constructs a child process, while clone-with-thread creates another
thread in the current group. `vfork` temporarily shares the parent address
space and blocks the parent. Normal child exec or exit releases that state. A
killable interruption of the parent wait can instead detach the parent. If the
child still uses the shared page directory then, it takes ownership; if exec
has already installed a new directory, the parent retains the old shared one.
Exec resolves this ownership before freeing its old image: a nonzero
`process_vfork_exec()` result means the old page directory belongs to the
parent and must not be freed by the child.
`execve` first quiesces sibling threads, then either commits a fully prepared
ELF image or restores the old process state. A failed image load therefore
does not expose a partially replaced address space.

## Signals, terminal control, and waiting

Signals may interrupt an interruptible wait and are delivered at the
user-return boundary. Process-directed signals select a suitable thread;
thread-directed signals remain private. Default exit and stop actions publish
the appropriate child event and wake a parent waiter. A stopped foreground
process group is associated with a controlling TTY; terminal-generated
signals are sent to that group.

`wait4` rechecks child state after every wakeup. It returns a reaped PID, zero
for a nonblocking no-event query, or an error for interruption or an invalid
selection. Reparenting assigns surviving children to init and sends HUP/CONT
when it leaves an orphaned stopped process group.

## Validation and limits

The QEMU runtime suite exercises init and exec, fork/vfork, clone, wait,
signals, process groups, sessions, terminal foreground control, credentials
including set-ID exec, and procfs snapshots. It runs from
`make -C tests qemu`. The model has a fixed maximum number of processes and
threads per process; it does not yet provide namespaces, cgroups,
capabilities, ptrace, or POSIX timers beyond the implemented real-time
interval timer.
