#ifndef __CAFFEINIX_KERNEL_DEBUG_H
#define __CAFFEINIX_KERNEL_DEBUG_H

#define DEBUG

#ifdef DEBUG

extern void panic(char* s);
#define PANIC(s) do {           \
    panic(s);                   \
} while (0)

#else

#define PANIC(s)    

#endif

#endif