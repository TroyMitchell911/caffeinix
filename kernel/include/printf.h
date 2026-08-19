#ifndef __CAFFEINIX_KERNEL_PRINTF_H
#define __CAFFEINIX_KERNEL_PRINTF_H

#include <stdarg.h>
#include <typedefs.h>
#include <console.h>

typedef void (*printf_emit_t)(int character, void *context);

void printf_init(void);
void printf(char* fmt, ...);
void printf_emergency(char *fmt, ...);
void vprintf_emit(printf_emit_t emit, void *context, const char *fmt,
		  va_list arguments);
void printf_enter_panic(void);

#endif
