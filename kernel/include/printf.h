#ifndef __CAFFEINIX_KERNEL_PRINTF_H
#define __CAFFEINIX_KERNEL_PRINTF_H

#include <typedefs.h>
#include <console.h>

void printf_init(void);
void printf(char* fmt, ...);

#endif