#ifndef __CAFFEINIX_ARCH_RISCV_UART_H
#define __CAFFEINIX_ARCH_RISCV_UART_H

#include <mem_layout.h>

void uart_init(void);
void uart_putc(int c);
void uart_puts(const char* s);

#endif