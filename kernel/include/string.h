#ifndef __CAFFEINIX_KERNEL_STRING_H
#define __CAFFEINIX_KERNEL_STRING_H

#include <typedefs.h>

void* memset(void* dst, char c, uint32 n);
size_t strlen(const char* s);
char* strncpy(char* s, const char* t, uint16 n);
char* safe_strncpy(char* s, const char* t, uint16 n);
void* memmove(void *dst, const void *src, uint16 n);

#endif