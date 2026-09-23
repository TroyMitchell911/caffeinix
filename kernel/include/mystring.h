/*
 * Kernel-local string and memory interfaces. Callers provide accessible
 * storage and serialize concurrent writes; these are not fault-catching
 * user-copy helpers.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#ifndef __CAFFEINIX_KERNEL_MYSTRING_H
#define __CAFFEINIX_KERNEL_MYSTRING_H

#include <typedefs.h>

void* memset(void* dst, int c, size_t n);
size_t strlen(const char* s);
char* strcpy(char* s, const char* t);
char* strncpy(char* s, const char* t, size_t n);
char* safe_strncpy(char* s, const char* t, size_t n);
void* memmove(void *dst, const void *src, size_t n);
void* memcpy(void *dst, const void *src, size_t n);
void* memchr(const void *buffer, int character, size_t n);
int memcmp(const void *p, const void *q, size_t n);
int strcmp(const char *p, const char *q);
int strncmp(const char *p, const char *q, size_t n);
char* strcat(char *p, const char *q);
char* strchr(const char *p, int c);
char* strrchr(const char *p, int c);
size_t strnlen(const char *s, size_t max);
void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *));
int atoi(const char *string);

#endif
