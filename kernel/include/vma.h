/*
 * Ordered user virtual-memory-area metadata.
 *
 * VMA APIs only change interval metadata and references; callers synchronize
 * corresponding page-table updates and hold mmap_lock for live processes.
 */
#ifndef __CAFFEINIX_KERNEL_VMA_H
#define __CAFFEINIX_KERNEL_VMA_H

#include <list.h>
#include <typedefs.h>

struct vfs_file;

struct vma_backing {
	void (*get)(struct vma_backing *backing);
	void (*put)(struct vma_backing *backing);
};

enum vma_origin {
	VMA_ANONYMOUS,
	VMA_FILE_BACKED,
};

enum vma_usage {
	VMA_ELF,
	VMA_HEAP,
	VMA_STACK,
	VMA_MMAP,
};

struct vm_area {
	struct list node;
	uint64 start;
	uint64 end;
	uint64 offset;
	uint64 file_length;
	uint32 protection;
	uint32 flags;
	enum vma_origin origin;
	enum vma_usage usage;
	struct vfs_file *file;
	struct vma_backing *backing;
};

struct vma_set {
	struct list areas;
};

/*
 * VMA invariants:
 *
 * - intervals are non-empty, page-aligned, ordered, and non-overlapping;
 * - adjacent intervals with identical attributes are merged;
 * - file-backed intervals own a file reference and a page-aligned offset;
 * - ELF file-backed intervals deny writes for their complete lifetime;
 * - shared anonymous intervals own a backing reference and offset;
 * - a live process serializes access with its mmap_lock; temporary exec
 *   sets and unpublished processes are owned exclusively by their caller.
 */
/**
 * vma_set_init() - Initialize an empty VMA set.
 * @set: Destination set.
 *
 * Context: Exclusive ownership; does not sleep.
 */
void vma_set_init(struct vma_set *set);
/**
 * vma_set_destroy() - Release all VMA and backing references.
 * @set: Set to empty.
 *
 * Context: Exclusive ownership or caller-held mmap lock; may free memory.
 */
void vma_set_destroy(struct vma_set *set);
/**
 * vma_set_move() - Transfer all areas, leaving @source empty.
 * @destination: Empty destination.
 * @source: Exclusively owned source.
 *
 * Context: Exclusive ownership; does not sleep.
 */
void vma_set_move(struct vma_set *destination, struct vma_set *source);
/**
 * vma_set_clone() - Deep-copy VMA metadata and acquire backing references.
 * @destination: Empty destination.
 * @source: Set to clone.
 *
 * Context: Source is stable; may allocate. Destination is safe to destroy on
 * failure.
 * Return: %0 or %-1; on failure @destination is safe to destroy.
 */
int vma_set_clone(struct vma_set *destination,
		  const struct vma_set *source);

/**
 * vma_insert() - Insert a page-aligned non-overlapping VMA.
 * @set: Destination.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * @protection: Linux PROT bits.
 * @flags: Mapping flags.
 * @origin: Backing kind.
 * @usage: Lifetime policy.
 * @file: File for file mappings, otherwise NULL.
 * @offset: Page-aligned file offset.
 * Return: %0 or %-1; success acquires required file references.
 */
int vma_insert(struct vma_set *set, uint64 start, uint64 end,
	       uint32 protection, uint32 flags, enum vma_origin origin,
	       enum vma_usage usage, struct vfs_file *file, uint64 offset);
/**
 * vma_insert_backed() - Insert a shared-anonymous backed interval.
 * @set: Destination.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * @protection: PROT bits.
 * @flags: Mapping flags.
 * @usage: Lifetime policy.
 * @backing: Referenced backing.
 * @offset: Page-aligned backing offset.
 * Return: %0 or %-1; success acquires a backing reference.
 */
int vma_insert_backed(struct vma_set *set, uint64 start, uint64 end,
		      uint32 protection, uint32 flags,
		      enum vma_usage usage, struct vma_backing *backing,
		      uint64 offset);
/**
 * vma_insert_elf() - Insert an immutable ELF file segment.
 * @set: Destination.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * @protection: Segment permissions.
 * @file: Executable file.
 * @offset: Offset.
 * Return: %0 or %-1; success pins the executable file mapping.
 */
int vma_insert_elf(struct vma_set *set, uint64 start, uint64 end,
		   uint32 protection, struct vfs_file *file, uint64 offset);
int vma_insert_elf_file(struct vma_set *set, uint64 start, uint64 end,
			 uint32 protection, struct vfs_file *file,
			 uint64 offset, uint64 file_length);
/**
 * vma_find() - Find the VMA containing an address.
 * @set: Set to query.
 * @address: Virtual address.
 *
 * Context: Set remains stable for the lookup.
 * Return: Borrowed VMA pointer or NULL; it remains valid while @set is locked.
 */
const struct vm_area *vma_find(const struct vma_set *set, uint64 address);
/**
 * vma_range_free() - Test that [@start, @end) overlaps no VMA.
 * @set: Set to query.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * Return: Nonzero when the interval is free.
 */
int vma_range_free(const struct vma_set *set, uint64 start, uint64 end);
int vma_range_mapped(const struct vma_set *set, uint64 start, uint64 end);
/**
 * vma_find_gap() - Find a free interval in a bounded address window.
 * @set: Set to query.
 * @low: Inclusive lower bound.
 * @high: Exclusive upper bound.
 * @hint: Preferred address.
 * @length: Required bytes.
 * @address: Result address.
 * Return: %0 or %-1 when no suitable gap exists.
 */
int vma_find_gap(const struct vma_set *set, uint64 low, uint64 high,
		 uint64 hint, uint64 length, uint64 *address);
int vma_find_gap_aligned(const struct vma_set *set, uint64 low, uint64 high,
			 uint64 hint, uint64 length, uint64 alignment,
			 uint64 align_offset, uint64 *address);
/**
 * vma_unmap() - Remove or split every interval overlapping [@start, @end).
 * @set: Mutable set.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * Return: %0 or %-1; failure preserves representable pre-existing fragments.
 */
int vma_unmap(struct vma_set *set, uint64 start, uint64 end);
/**
 * vma_protect() - Change protection metadata for a mapped interval.
 * @set: Mutable set.
 * @start: Inclusive address.
 * @end: Exclusive address.
 * @protection: New PROT bits. Return: %0 or %-1 for an invalid range.
 */
int vma_protect(struct vma_set *set, uint64 start, uint64 end,
		uint32 protection);
/**
 * vma_count() - Count VMA entries.
 * @set: Set to query.
 *
 * Context: Set remains stable for the lookup.
 * Return: Entry count.
 */
int vma_count(const struct vma_set *set);

#endif
