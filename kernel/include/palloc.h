#ifndef __CAFFEINIX_KERNEL_PALLOC_H
#define __CAFFEINIX_KERNEL_PALLOC_H

#include <typedefs.h>
#include <debug.h>
#include <riscv.h>

#define PALLOC_ZERO 0x1

void palloc_init(void);
void *alloc_pages(unsigned int order, unsigned int flags);
void free_pages(void *p, unsigned int order);
void pfree(void* p);
void* palloc(void);
void *palloc_zero(void);
int palloc_get(void *p);
uint32 palloc_refcount(void *p);
int palloc_managed_range_count(void);
int palloc_managed_range_get(int index, uint64 *start, uint64 *end);
uint64 palloc_heap_start(void);
uint64 palloc_usable_bytes(void);
int palloc_reference_selftest(void);

void* malloc(uint64 size);
void* calloc(size_t count, size_t size);
void free(void* p);

#endif
