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