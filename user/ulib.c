/*
 * @Author: TroyMitchell
 * @Date: 2024-05-14
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-31
 * @FilePath: /caffeinix/user/ulib.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "stat.h"
#include "fcntl.h"
#include "user.h"

void _main(void)
{
        extern int main();
        main();
        exit(0);
}
/**  
 * @description: Copies a string  
 * Copies the string pointed to by `t` (including the terminating null character) into the array  
 * pointed to by `s`. The copying stops when the terminating null character is reached.  
 * @param {char~} s Pointer to the destination array where the content is to be copied  
 * @param {char} t Pointer to the source string to be copied  
 * @return Returns a pointer to the destination string `s`  
 */ 
char* strcpy(char *s, const char *t)
{
        char *os;

        os = s;
        while((*s++ = *t++) != 0)
                ;
        return os;
}

/**  
 * @description:  Copies a string with length limitation  
 * Copies the first `n` characters of the string pointed to by `t` (not including the terminating null character)  
 * into the array pointed to by `s`. If the string pointed to by `t` is shorter than `n`, null characters are appended  
 * to the copy to ensure the total number of characters copied is `n`. The original `s` is always null-terminated.  
 * @param {char*} s Pointer to the destination array where the content is to be copied  
 * @param {char*} t Pointer to the source string to be copied  
 * @param {short} n Number of characters to be copied  
 * @return Returns a pointer to the destination string `s`  
 */  
char* strncpy(char* s, const char* t, unsigned short n)
{
        char *os;       

        os = s;
        while(n-- > 0 && (*s++ = *t++) != 0)
                ;
        while(n-- > 0)
                *s++ = 0;
        return os;  
}

void* memmove(void *dst, const void *src, int n)
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

/**
 * @description: Added a function that Finds the first occurrence of a given character in a string
 * @param {char*} The string to search for
 * @param {char} The character to look for
 * @return {char*} returns the position of the first occurrence of the character c in s, or 0 if not found
 */
char* strchr(const char* s, char c)
{
        for(; *s; s++) {
                if(*s == c) {
                        return (char*)s;
                }
        }
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

extern int read(int, void*, int);
char* gets(char* buf, int max)
{
        int ret;
        ret = read(0, buf, max - 2);
        if(ret == -1)
                return 0;

        /* Overflow process*/
        if(buf[ret - 1] != '\n') {
                buf[ret] = '\n';
                ret ++;
                printf("\n");
        }

        buf[ret] = '\0';
        return buf;
}

/* Get string length */
unsigned int strlen(const char* s)
{
        char* p = (char*)s;
        while((*p++) != '\0');
        return (p - s - 1);
}

/* Clear n bytes of memory pointing to dst as c */
void* memset(void* dst, int c, unsigned int n)
{
        char* d = (char*)dst;
        int i;
        
        for(i = 0; i < n; i++) {
                d[i] = c;
        }

        return dst;
}

/**
 * @description: Converts the string s to an integer
 * @param {char *} The transformed string
 * @return {int} Converted integer
 */
int atoi(const char *s)
{
        int n;

        n = 0;
        while(*s >= '0' && *s <= '9')
                n = n*10 + *s++ - '0';

        return n;
}

/**
 * @description: Compare the first n bytes of the memory region pointed to by s1 and s2
 * @param {void} *s1: Points to the compared memory area s1
 * @param {void} *s2: Points to the compared memory area s2
 * @param {unsigned int} n: Compare the first n bytes
 * @return {int}: Returns the difference between the first distinct byte (as an int). If two memory regions are identical, 0 is returned
 */
int memcmp(const void *s1, const void *s2, unsigned int n)
{
        const char *p1 = s1, *p2 = s2;

        while (n-- > 0) {
                if (*p1 != *p2) {
                        return (int)(*p1 - *p2);
                }
                p1++;
                p2++;
        }

        return 0;
}

void* memcpy(void* dst, const void* src, unsigned int n)
{
        return memmove(dst, src, n);
}

/**  
 *@description:
 * This function retrieves the status information of a file given its path `path` and stores it in the `struct stat` `st`.  
 * @param {char} path Path to the file for which status information is to be retrieved  
 * @param {stat} st Pointer to a `struct stat` where the file status information will be stored  
 * @return Returns 0 on success, -1 on failure  
 */
int stat(const char *path, struct stat *st)
{
        int fd, ret;

        fd = open(path, O_RDONLY);
        if(fd == -1)
                return -1;
        
        ret = fstat(fd, st);
        close(fd);
        
        return ret;
}

void strcat(char *p, const char *q)
{
        while(*p != '\0') p++;
        while(*q != '\0') *p++ = *q++;
        *p = '\0';
}



