#include <string.h>

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