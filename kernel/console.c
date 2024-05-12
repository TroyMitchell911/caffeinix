/*
 * @Author: TroyMitchell
 * @Date: 2024-04-26
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-12
 * @FilePath: /caffeinix/kernel/console.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <console.h>
#include <spinlock.h>
#include <driver.h>
#include <process.h>

static struct {
        struct spinlock lock;
  
        #define INPUT_BUF_SIZE 128
        char buf[INPUT_BUF_SIZE];
        /* read index */
        uint32 r;  
        /* write index */
        uint32 w;  
        /* edit index */
        uint32 e;  
}console;

#define BACKSPACE       0x100
#define C(x)            ((x)-'@')  // Control-x

static void console_intr(int c)
{
        spinlock_acquire(&console.lock);

        switch(c) {
                default:
                        if(c != 0 && console.e - console.r < INPUT_BUF_SIZE) {
                                c = (c == '\r') ? '\n' : c;
                                /* Echo */
                                console_putc(c);
                                console.buf[console.e++ % INPUT_BUF_SIZE] = c;

                                if(c == '\n' || console.e - console.r == INPUT_BUF_SIZE) {
                                        console.w = console.e;
                                        /* wakeup something */
                                }
                        }
                        break;
        }

        spinlock_release(&console.lock);
}

void console_putc(int c)
{
        if(c == BACKSPACE) {
                /* Clear the character */
                uart_putc_sync('\b');
                uart_putc_sync(' ');
                uart_putc_sync('\b');
        } else {
                uart_putc_sync(c);
        }
}

#include <debug.h>

int console_read(uint64 dst, int n)
{

        return 0;
}

int console_write(uint64 src, int n)
{
        int i, ret;
        char c;
        for(i = 0; i < n; i++) {
                ret = either_copyin(&c, 1, src + i, 1);
                if(ret != 0) {
                        break;
                }
                uart_putc(c);
        }
        return i;
}

void console_init(void)
{
        spinlock_init(&console.lock, "console");
        uart_init();
        uart_register_rx_callback(console_intr);

        /* Set the operating function */
        dev[CONSOLE].write = console_write;
        dev[CONSOLE].read = console_read;
}