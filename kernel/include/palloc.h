#ifndef __CAFFEINIX_KERNEL_PALLOC_H
#define __CAFFEINIX_KERNEL_PALLOC_H

#include <typedefs.h>
#include <debug.h>
#include <riscv.h>

void palloc_init(void);
void pfree(void* p);
void* palloc(void);

void* malloc(uint64 size);
void free(void* p);

#endif