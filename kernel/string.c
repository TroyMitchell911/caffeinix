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
void* memset(void* dst, char c, uint32 n)
{
        char* d = (char*)dst;
        int i;
        
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

char* strncpy(char* s, const char* t, uint16 n)
{
        char *os;       

        os = s;
        while(n-- > 0 && (*s++ = *t++) != 0)
                ;
        while(n-- > 0)
                *s++ = 0;
        return os;  
}

char* safe_strncpy(char* s, const char* t, uint16 n)
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

void* memmove(void *dst, const void *src, uint16 n)
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

void* memcpy(void* dst, const void* src, uint16 n)
{
        return memmove(dst, src, n);
}

int strncmp(const char *p, const char *q, uint32 n)
{
        while(n > 0 && *p && *p == *q)
                n--, p++, q++;
        if(n == 0)
                return 0;
        return (uint8)*p - (uint8)*q;
}

void strcat(char *p, const char *q)
{
        while(*p != '\0') p++;
        while(*q != '\0') *p++ = *q++;
        *p = '\0';
}

char* strchr(const char *p, int c)
{
        for(; *p != '\0' && *p != c; p++);

        return (char*)p;
}

char* strrchr(const char *p, int c)
{
        const char *p_start = p;

        for(; *p != '\0'; p++);
        for(; p > p_start && *p != c; p--);

        return p == p_start && *p != c ? 0 : (char*)p;
}