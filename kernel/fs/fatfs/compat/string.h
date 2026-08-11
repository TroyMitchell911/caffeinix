#ifndef CAFFEINIX_FATFS_STRING_H
#define CAFFEINIX_FATFS_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
char *strchr(const char *string, int character);

#endif
