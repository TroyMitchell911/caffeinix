/*
 * Anonymous shared VMA backing.
 *
 * This interface owns the pages shared by MAP_SHARED anonymous mappings.
 * VMA lifetime is represented by vma_backing references; each returned page
 * carries a separate allocator reference for the faulting mapping.
 */
#ifndef __CAFFEINIX_KERNEL_ANON_MAPPING_H
#define __CAFFEINIX_KERNEL_ANON_MAPPING_H

#include <typedefs.h>

struct vma_backing;

/**
 * anon_mapping_create() - Create backing storage for a shared anonymous VMA.
 *
 * Context: Thread context; may allocate and sleep.
 *
 * Return: A backing with one caller-owned reference, or NULL on allocation
 * failure. The caller releases it through its vma_backing @put method.
 */
struct vma_backing *anon_mapping_create(void);

/**
 * anon_mapping_get_page() - Look up or allocate one shared anonymous page.
 * @backing: Backing returned by anon_mapping_create().
 * @offset: Page-aligned byte offset within @backing.
 * @page: Receives a page with one caller-owned allocator reference.
 *
 * Context: Thread context; may allocate, reclaim cache pages, and sleep.
 * The backing remains referenced for the complete call.
 *
 * Return: %0 on success or %-1 for invalid arguments or allocation failure.
 */
int anon_mapping_get_page(struct vma_backing *backing, uint64 offset,
			  void **page);

#endif
