# Freestanding kernel helpers

The kernel does not link a userspace libc. Common helpers in `kernel/` supply
the subset needed by first-party code and imported-library adapters. GCC
builtin headers provide compiler types; the Linux or musl sysroot does not
define kernel object layouts. User programs use musl independently.

## Intrusive containers

`list.h` implements circular doubly linked lists. Empty heads and detached
entries point to themselves. Insertion requires a detached entry; removal
rewires the neighbors and reinitializes the entry without freeing its
containing object. A list stores no ownership or lock, so its caller must
exclude concurrent mutations and keep objects alive while traversing.

`rbtree.h` and `rbtree.c` separate key policy from balancing. The caller
searches using its own key and duplicate policy, links a red leaf with
`rb_link_node()`, then calls `rb_insert_color()` before publishing the change.
Erase restores balance and resets the removed node; it does not free or copy
objects. `rb_first()` and `rb_next()` return borrowed nodes. Holding a pointer
does not protect it from a concurrent erase or object destruction.

The scheduler embeds these nodes in its scheduling entities. The generic
tree has no scheduling policy, allocation, or locking; callers choose their
own synchronization. Host `tests/rbtree.c` checks ordering, parent links,
colors, and black height across deterministic randomized mutations.

## Bytes and strings

The functions in `mystring.h` operate on accessible kernel memory. They do
not catch user faults and must never replace `copyin`, `copyout`, or
`copyinstr` for untrusted user addresses. None provides internal locking.

`memmove()` handles overlapping ranges. The current `memcpy()` delegates to
it, but new callers should still use `memmove()` when overlap is intended.
Bounded routines use byte lengths, not element counts or Unicode character
counts. `safe_strncpy()` reserves a NUL byte whenever capacity is nonzero;
it does not pad the entire remaining buffer.

The legacy `strncpy()` has an existing unsigned-count underflow if no NUL is
seen before its limit, including a zero limit. Its current fixed-string call
fits; do not use it for truncation. This is a known implementation limitation,
not standard libc `strncpy` semantics. Fixing it needs a separate behavioral
patch and boundary tests.

`atoi()` accepts a small signed decimal value with optional leading spaces
or tabs. It has no overflow/error reporting. `qsort()` is actually insertion
sort, using byte swaps and the caller's comparator; it allocates nothing but
has quadratic worst-case work. It is intended for small tables, not a large
unbounded userspace workload.

## Allocation and imported code

Physical pages, heap objects, files, and subsystem objects have different
lifetimes. Pair allocations with their own release APIs; `free()` is not
interchangeable with `pfree()`, and container removal does not release an
object reference. See [memory management](memory-management.md).

Local compatibility headers connect imported libraries to this limited
freestanding environment. A familiar libc name is not evidence that every
libc behavior is implemented. Preserve imported sources and document missing
requirements in the adapter rather than pulling user libc into the kernel.

## Validation

`make -C tests rbtree` runs the host container test. `make -C tests qemu`
exercises the helpers through real scheduler, filesystem, network, exec,
allocator, and userspace-copy workloads. That integration coverage does not
constitute exhaustive libc conformance testing.
