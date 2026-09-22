# Memory management

Caffeinix uses RISC-V SV39 page tables for user processes and one kernel
address space.  This document describes the first-party memory-management
components; API contracts and exact error returns remain beside the code.

## Physical memory

At boot, `palloc_init()` reads usable RAM from the device tree, normalizes it
with `memrange_set`, and excludes the kernel image and allocator metadata.
The remaining disjoint page-aligned regions are given to `buddy_allocator`.
Its free lists allocate `2^order` contiguous pages. `page_lock` serializes
buddy state and the out-of-line order-zero reference counters. Single-page
allocations start with one reference; `palloc_get()` and `pfree()` acquire and
release such references. Higher-order allocations remain indivisible blocks
and must be released with their original order. The allocator itself does not
sleep or reclaim; fault/cache paths decide when to reclaim and retry.

`alloc_pages()` and `palloc_zero()` return NULL on exhaustion. The legacy
`palloc()` wrapper instead panics; callers cannot recover through a NULL check
after that wrapper.

The small kernel heap is separate from page allocation.  It takes heap pages
from the physical allocator and protects bitmap allocation with `heap_lock`.
Callers must not mix `free()` with `pfree()` or `free_pages()`.

## Virtual address spaces and VMAs

`vm_area` entries are sorted, page-aligned, non-overlapping half-open
intervals.  They describe permissions, mapping origin, and the backing file
or shared-anonymous object.  A live process holds `mmap_lock` while changing
its VMA set or page tables.  Temporary exec sets and unpublished processes
are exclusively owned by their caller.

`vm.c` walks and creates SV39 page tables.  It can select huge leaves only
when virtual address, physical address, and remaining size agree.  Fork
clones eligible pages with copy-on-write protection; a write fault resolves
the private copy.  Page-table changes must invalidate the relevant TLB state
through the architecture helpers used by the VM layer.

## Faults and backing storage

`mmap_handle_fault()` checks the VMA and requested access before selecting a
backing source:

* File-backed pages use the global page cache.  Cache entries retain a VFS
  file and a physical page; users of a returned page hold an extra page ref.
* Shared anonymous mappings use a sparse `vma_backing` object.  Its sleeplock
  serializes page lookup and creation, and the backing reference count keeps
  it alive across split and cloned VMAs.
* Private writable mappings can become copy-on-write pages after fork.

Fault handling may allocate, reclaim cache pages, perform filesystem I/O, and
sleep.  Trap code must therefore invoke it only from process context, not from
hard interrupt context.  The result distinguishes unmapped, permission,
I/O, memory-pressure, and retry cases so trap handling can choose the proper
signal or retry path.

## Cache writeback and reclaim

The page-cache sleeplock protects entry membership and accounting and remains
held during cache-miss reads, readahead, and writeback I/O. Writeback takes the
superblock `write_lock` before the cache lock; backend I/O locks come after the
cache lock. Backend code must not re-enter the cache while holding those locks.

Ordinary reclaim skips dirty, writeback-mapped, evicting, or externally
referenced pages. Mapped reclaim marks an entry `evicting`, drops the cache lock
before asking mmap code to remove mappings, then reacquires it to recheck the
entry and its page references. It frees the entry only when it is still clean
and the cache holds the sole page reference. This lock-dropping path must not
be confused with cache-fill or writeback I/O, which stays under the lock.

Truncation invalidates cache and mapped-file pages beyond the new file size.
The filesystem must call the truncate and writeback hooks around changes that
would otherwise leave stale data visible through mappings.

## Limits and tests

`MEMRANGE_MAX` bounds discovered physical regions and `BUDDY_MAX_ORDER`
bounds a single contiguous allocation.  User space is limited by the SV39
layout in `arch/riscv/include/mem_layout.h`, including separate randomized
image, interpreter, heap, mmap, stack, trampoline, and trapframe windows.

Run `make -C tests qemu` for the runtime suite.  Its memory tests cover mmap
permissions, shared and private mappings, copy-on-write, truncation, cache
writeback/reclaim, and allocator reference checks. The CI matrix adds boot
checks with multiple hart counts and memory sizes; the exhaustive integration
workload runs separately with eight harts, rather than at every matrix point.
