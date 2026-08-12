#ifndef __CAFFEINIX_KERNEL_PALLOC_H
#define __CAFFEINIX_KERNEL_PALLOC_H

#include <typedefs.h>
#include <debug.h>
#include <riscv.h>

void palloc_init(void);
void pfree(void* p);
void* palloc(void);
int palloc_memory_range_count(void);
int palloc_memory_range_get(int index, uint64 *start, uint64 *end);
int palloc_page_usable(uint64 address);
uint64 palloc_heap_start(void);

void* malloc(uint64 size);
void* calloc(size_t count, size_t size);
void free(void* p);

#endif
