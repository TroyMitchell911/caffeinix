#ifndef LWEXT4_COMPAT_STDLIB_H
#define LWEXT4_COMPAT_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *));

#endif
