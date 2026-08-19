#ifndef __CAFFEINIX_KERNEL_ANON_MAPPING_H
#define __CAFFEINIX_KERNEL_ANON_MAPPING_H

#include <typedefs.h>

struct vma_backing;

struct vma_backing *anon_mapping_create(void);
int anon_mapping_get_page(struct vma_backing *backing, uint64 offset,
			  void **page);

#endif
