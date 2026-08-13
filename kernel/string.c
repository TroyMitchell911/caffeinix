/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-30
 * @FilePath: /caffeinix/kernel/string.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <mystring.h>

/* Clear n bytes of memory pointing to dst as c */
void* memset(void* dst, int c, size_t n)
{
        char* d = (char*)dst;
	size_t i;
        
        for(i = 0; i < n; i++) {
                d[i] = c;
        }

        return dst;
}

/* Get string length */
size_t strlen(const char* s)
{
        char* p = (char*)s;
        while((*p++) != '\0');
        return (p - s - 1);
}

char* strcpy(char* s, const char* t)
{
	char *original = s;

	while ((*s++ = *t++))
		;
	return original;
}

char* strncpy(char* s, const char* t, size_t n)
{
        char *os;       

        os = s;
        while(n-- > 0 && (*s++ = *t++) != 0)
                ;
        while(n-- > 0)
                *s++ = 0;
        return os;  
}

char* safe_strncpy(char* s, const char* t, size_t n)
{
        char *os;

        os = s;
        if(n <= 0)
                return os;
        while(--n > 0 && (*s++ = *t++) != 0)
                ;
        *s = 0;
        return os;  
}

void* memmove(void *dst, const void *src, size_t n)
{
        const char *s;
        char *d;

        if(n == 0)
                return dst;

        s = src;
        d = dst;
        if(s < d && s + n > d){
                s += n;
                d += n;
                while(n-- > 0)
                *--d = *--s;
        } else
                while(n-- > 0)
                        *d++ = *s++;

        return dst;
}

void* memcpy(void* dst, const void* src, size_t n)
{
        return memmove(dst, src, n);
}

void* memchr(const void *buffer, int character, size_t n)
{
	const uint8 *bytes = buffer;

	while (n--) {
		if (*bytes == (uint8)character)
			return (void *)bytes;
		bytes++;
	}
	return 0;
}

int memcmp(const void *left, const void *right, size_t n)
{
	const uint8 *p = left;
	const uint8 *q = right;

	while (n--) {
		if (*p != *q)
			return *p - *q;
		p++;
		q++;
	}
	return 0;
}

int strcmp(const char *p, const char *q)
{
	while (*p && *p == *q) {
		p++;
		q++;
	}
	return (uint8)*p - (uint8)*q;
}

int strncmp(const char *p, const char *q, size_t n)
{
        while(n > 0 && *p && *p == *q)
                n--, p++, q++;
        if(n == 0)
                return 0;
        return (uint8)*p - (uint8)*q;
}

char* strcat(char *p, const char *q)
{
	char *original = p;

        while(*p != '\0') p++;
        while(*q != '\0') *p++ = *q++;
        *p = '\0';
	return original;
}

char* strchr(const char *p, int c)
{
	for (;; p++) {
		if (*p == c)
			return (char *)p;
		if (!*p)
			return 0;
	}
}

char* strrchr(const char *p, int c)
{
        const char *p_start = p;

        for(; *p != '\0'; p++);
        for(; p > p_start && *p != c; p--);

	return p == p_start && *p != c ? 0 : (char*)p;
}

size_t strnlen(const char *s, size_t max)
{
	size_t length = 0;

	while (length < max && s[length])
		length++;
	return length;
}

static void byte_swap(uint8 *left, uint8 *right, size_t size)
{
	while (size--) {
		uint8 value = *left;

		*left++ = *right;
		*right++ = value;
	}
}

void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *))
{
	uint8 *array = base;
	size_t i, j;

	if (!array || !size || count < 2)
		return;
	for (i = 1; i < count; i++) {
		for (j = i; j &&
		     compare(array + (j - 1) * size,
		             array + j * size) > 0; j--)
			byte_swap(array + (j - 1) * size,
			          array + j * size, size);
	}
}

int atoi(const char *string)
{
	int negative = 0, value = 0;

	while (*string == ' ' || *string == '\t')
		string++;
	if (*string == '-' || *string == '+')
		negative = *string++ == '-';
	while (*string >= '0' && *string <= '9')
		value = value * 10 + *string++ - '0';
	return negative ? -value : value;
}
