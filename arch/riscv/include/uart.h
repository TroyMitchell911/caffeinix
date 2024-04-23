#ifndef __CAFFEINIX_ARCH_RISCV_UART_H
#define __CAFFEINIX_ARCH_RISCV_UART_H

#include <mem_layout.h>

void uart_init(void);
void uart_putc(int c);
void uart_puts(const char* s);
void uart_putc_sync(int c);
void uart_intr(void);

typedef void (*uart_rx_callback_t)(int c);
void uart_register_rx_callback(uart_rx_callback_t callback);

#endif