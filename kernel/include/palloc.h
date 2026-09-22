/*
 * Physical-page allocation, page references, and the small kernel heap.
 *
 * Order-zero page allocations carry reference counts; higher-order blocks
 * have one exclusive owner and are released with their original order.
 * Heap objects must be returned with free(). These allocators take spinlocks
 * and do not sleep or initiate reclaim; higher memory-management layers
 * perform reclaim before retrying allocation.
 */
#ifndef __CAFFEINIX_KERNEL_PALLOC_H
#define __CAFFEINIX_KERNEL_PALLOC_H

#include <typedefs.h>
#include <debug.h>
#include <riscv.h>

#define PALLOC_ZERO 0x1

/**
 * palloc_init() - Discover usable RAM and initialize page and heap allocators.
 *
 * Context: Single-threaded early boot; runs once before any allocation.
 */
void palloc_init(void);
/**
 * alloc_pages() - Allocate 2^@order contiguous pages.
 * @order: Log2 page count, no greater than BUDDY_MAX_ORDER.
 * @flags: PALLOC_ZERO requests zero-filled pages.
 *
 * Context: Takes page_lock; does not sleep or reclaim. Only order zero has
 * a page reference count; other orders must be freed intact with free_pages().
 *
 * Return: Contiguous block, or NULL on invalid flags/order or exhaustion.
 */
void *alloc_pages(unsigned int order, unsigned int flags);
/**
 * free_pages() - Release an exact alloc_pages() allocation.
 * @p: Live allocation base; NULL is invalid.
 * @order: Original allocation order.
 *
 * Context: Caller owns the block exclusively; takes page_lock without sleep.
 * Order-zero pages must have exactly one reference. A bad page, order, or
 * reference count is allocator corruption and panics.
 */
void free_pages(void *p, unsigned int order);
/**
 * pfree() - Drop one reference to a single managed page.
 * @p: Page-aligned managed page address.
 *
 * Context: Any context that may take page_lock. The final drop returns it to
 * the buddy allocator.
 */
void pfree(void* p);
/**
 * palloc() - Allocate one physical page.
 *
 * Context: Takes page_lock; no sleep or reclaim.
 * Return: Page with reference count one and unspecified contents. Exhaustion
 * panics; this wrapper never returns NULL. Use alloc_pages() or palloc_zero()
 * when the caller must handle allocation failure.
 */
void* palloc(void);
/**
 * palloc_zero() - Allocate and clear one physical page.
 *
 * Context: Takes page_lock; no sleep or reclaim.
 * Return: Zeroed page with reference count one, or NULL on exhaustion.
 */
void *palloc_zero(void);
/**
 * palloc_get() - Acquire another reference to a managed page.
 * @p: Page-aligned managed page address.
 *
 * Context: Any context that may take page_lock. Does not sleep.
 * Return: %0 or %-1 for an unmanaged or dead page.
 */
int palloc_get(void *p);
/**
 * palloc_refcount() - Read a managed page's reference count.
 * @p: Page-aligned page address.
 *
 * Context: Any context. Does not sleep.
 * Return: Reference count, or zero for an unmanaged page.
 */
uint32 palloc_refcount(void *p);
/**
 * palloc_managed_range_count() - Return discovered managed-region count.
 *
 * Context: After palloc_init(); does not sleep.
 * Return: Number of managed physical intervals.
 */
int palloc_managed_range_count(void);
/**
 * palloc_managed_range_get() - Copy one managed physical interval.
 * @index: Region index.
 * @start: Receives inclusive address.
 * @end: Receives exclusive address.
 *
 * Context: After palloc_init(); does not sleep.
 * Return: %0 or %-1 for an invalid index or output pointer.
 */
int palloc_managed_range_get(int index, uint64 *start, uint64 *end);
/**
 * palloc_heap_start() - Return the first address reserved for heap metadata.
 *
 * Context: After palloc_init(); does not sleep.
 * Return: Physical/identity-mapped start address.
 */
uint64 palloc_heap_start(void);
/**
 * palloc_usable_bytes() - Return bytes admitted to the managed allocator.
 *
 * Context: After palloc_init(); does not sleep.
 * Return: Byte count.
 */
uint64 palloc_usable_bytes(void);
/**
 * palloc_free_pages() - Return currently free physical pages.
 *
 * Context: Any context that may take page_lock. Does not sleep.
 * Return: Page count.
 */
uint64 palloc_free_pages(void);
/**
 * palloc_reference_selftest() - Validate reference counting invariants.
 *
 * Context: Serialized boot selftest; allocates and releases temporary pages.
 * Return: %0 on success or %-1 on failure.
 */
int palloc_reference_selftest(void);

/**
 * malloc() - Allocate bytes from the kernel heap.
 * @size: Requested nonzero byte count.
 *
 * Context: Takes heap_lock, then page_lock if backing storage is needed;
 * neither path sleeps or initiates reclaim.
 *
 * Return: Aligned allocation released by free(), or NULL for zero, overflow,
 * exhaustion, or a request too large for one heap page with its metadata.
 */
void* malloc(uint64 size);
/**
 * calloc() - Allocate and clear an array.
 * @count: Element count.
 * @size: Bytes per element.
 *
 * Context: Same non-sleeping locking and single-page size limit as malloc().
 *
 * Return: Zeroed allocation, or NULL for zero size, overflow, or exhaustion.
 */
void* calloc(size_t count, size_t size);
/**
 * free() - Release a malloc()/calloc() allocation.
 * @p: Allocation pointer, or NULL.
 *
 * Context: Takes heap_lock and possibly page_lock; does not sleep. NULL is
 * ignored. The pointer must be a live heap allocation: metadata checks are
 * diagnostic and do not make an arbitrary or stale address safe to pass.
 */
void free(void* p);

#endif
