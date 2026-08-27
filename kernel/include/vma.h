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
void vma_set_init(struct vma_set *set);
void vma_set_destroy(struct vma_set *set);
void vma_set_move(struct vma_set *destination, struct vma_set *source);
int vma_set_clone(struct vma_set *destination,
		  const struct vma_set *source);

int vma_insert(struct vma_set *set, uint64 start, uint64 end,
	       uint32 protection, uint32 flags, enum vma_origin origin,
	       enum vma_usage usage, struct vfs_file *file, uint64 offset);
int vma_insert_backed(struct vma_set *set, uint64 start, uint64 end,
		      uint32 protection, uint32 flags,
		      enum vma_usage usage, struct vma_backing *backing,
		      uint64 offset);
int vma_insert_elf(struct vma_set *set, uint64 start, uint64 end,
		   uint32 protection, struct vfs_file *file, uint64 offset);
int vma_insert_elf_file(struct vma_set *set, uint64 start, uint64 end,
			 uint32 protection, struct vfs_file *file,
			 uint64 offset, uint64 file_length);
const struct vm_area *vma_find(const struct vma_set *set, uint64 address);
int vma_range_free(const struct vma_set *set, uint64 start, uint64 end);
int vma_range_mapped(const struct vma_set *set, uint64 start, uint64 end);
int vma_find_gap(const struct vma_set *set, uint64 low, uint64 high,
		 uint64 hint, uint64 length, uint64 *address);
int vma_find_gap_aligned(const struct vma_set *set, uint64 low, uint64 high,
			 uint64 hint, uint64 length, uint64 alignment,
			 uint64 align_offset, uint64 *address);
int vma_unmap(struct vma_set *set, uint64 start, uint64 end);
int vma_protect(struct vma_set *set, uint64 start, uint64 end,
		uint32 protection);
int vma_count(const struct vma_set *set);

#endif
