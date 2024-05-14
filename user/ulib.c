/*
 * @Author: TroyMitchell
 * @Date: 2024-05-14
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/user/ublic.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "../arch/riscv/include/typedefs.h"

char* strcpy(char *s, const char *t)
{
        char *os;

        os = s;
        while((*s++ = *t++) != 0)
                ;
        return os;
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

char* strchr(const char* s, char c)
{
        return 0;
}

int strcmp(const char *p, const char *q)
{
        while(*p && *p == *q)
                p++, q++;
        return (unsigned char)*p - (unsigned char)*q;
}


int strncmp(const char *p, const char *q, uint32 n)
{
        while(n > 0 && *p && *p == *q)
                n--, p++, q++;
        if(n == 0)
                return 0;
        return (uint8)*p - (uint8)*q;
}

char* gets(char* buf, int max)
{
        return 0;
}

/* Get string length */
size_t strlen(const char* s)
{
        char* p = (char*)s;
        while((*p++) != '\0');
        return (p - s - 1);
}

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

int atoi(const char *s)
{
        return 0;
}

int memcmp(const void *s1, const void *s2, unsigned int n)
{
        return 0;
}

void* memcpy(void* dst, const void* src, uint16 n)
{
        return memmove(dst, src, n);
}



