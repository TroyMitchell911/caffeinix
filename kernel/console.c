/*
 * @Author: TroyMitchell
 * @Date: 2024-04-26
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-15
 * @FilePath: /caffeinix/kernel/console.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "process.h"
#include <console.h>
#include <spinlock.h>
#include <driver.h>
#include <process.h>
#include <mystring.h>

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
                                /* We can wakeup the blocked process by read if the user input '\n' */
                                if(c == '\n' || console.e - console.r == INPUT_BUF_SIZE) {
                                        console.w = console.e;
                                        wakeup_(&console.r);
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

int console_read(uint64 dst, int n)
{
        char c;
        int ret, target;

        target = n;

        spinlock_acquire(&console.lock);

        while(n > 0) {
                /* Wait until the interrupt handler put some data into console */
                while(console.r == console.w) {
                        /* TODO: If this process is killed */
                        sleep_(&console.r, &console.lock);
                }

                c = console.buf[console.r++ % INPUT_BUF_SIZE];

                /* TODO: other character */

                ret = either_copyout(1, dst, &c, 1);
                if(ret != 0) {
                        break;
                }

                n --;
                dst ++;

                if(c == '\n') {
                        break;
                }
        }

        spinlock_release(&console.lock);

        return target - n;
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