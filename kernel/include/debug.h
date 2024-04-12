#ifndef __CAFFEINIX_KERNEL_DEBUG_H
#define __CAFFEINIX_KERNEL_DEBUG_H

#define DEBUG

#ifdef DEBUG

#define PANIC(s) do {           \
    for(;;);                    \
} while (0)


#else

#endif

#endif