/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-07
 * @FilePath: /caffeinix/kernel/include/mystring.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_MYSTRING_H
#define __CAFFEINIX_KERNEL_MYSTRING_H

#include <typedefs.h>

void* memset(void* dst, char c, uint32 n);
size_t strlen(const char* s);
char* strncpy(char* s, const char* t, uint16 n);
char* safe_strncpy(char* s, const char* t, uint16 n);
void* memmove(void *dst, const void *src, uint16 n);
int strncmp(const char *p, const char *q, uint32 n);

#endif