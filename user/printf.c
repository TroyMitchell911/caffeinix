/*
 * @Author: TroyMitchell
 * @Date: 2024-05-14
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/user/printf.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include <stdarg.h>

#include "user.h"
#include "../arch/riscv/include/typedefs.h"

static char digits[] = "0123456789ABCDEF";

static void putc(int fd, char c)
{
        write(fd, &c, 1);
}

static void print_int(int fd, int number, uint8 base, uint8 sign)
{
        char buf[16];
        int i;
        uint32 num;
        /* If the flag 'sign' == 1 and the number is negative */
        if(sign && (sign = number < 0))
                num = -number;
        else
                num = number;

        i = 0;
        /* Get each digit into the buffer 'buf' */
        do {
                buf[i++] = digits[num % base];
        } while((num /= base) != 0);
        /* We should add a negative symbol at the end if the sign == 1 */
        if(sign)
                buf[i++] = '-';
        /* Inverted output */
        while(--i >= 0)
                putc(fd, buf[i]);
}

/* Print a pointer */
static void print_ptr(int fd, uint64 ptr)
{
        int i;
        putc(fd, '0');
        putc(fd, 'x');
        /* Output every 4 bits as a number */
        for (i = 0; i < (sizeof(uint64) * 2); i++, ptr <<= 4)
                putc(fd, digits[ptr >> (sizeof(uint64) * 8 - 4)]);
}

static void vprintf(int fd, const char* fmt, va_list ap)
{
        int i, c;
        char *s;

        for(i = 0; (c = fmt[i] & 0xff) != 0; i++) {
                /* We directly output if no converting symbol */
                if(c != '%'){
                        putc(fd, c);
                        continue;
                }
                /* Get next character if converting symbol */
                c = fmt[++i] & 0xff;
                switch(c) {
                        /* Dec */
                        case 'd':
                                print_int(fd, va_arg(ap, int), 10, 1);
                                break;
                        /* Hex */
                        case 'x':
                                print_int(fd, va_arg(ap, int), 16, 1);
                                break;
                        /* Pointer */
                        case 'p':
                                print_ptr(fd, va_arg(ap, uint64));
                                break;
                        /* String */
                        case 's':
                                if((s = va_arg(ap, char*)) == 0)
                                s = "(null)";
                                for(; *s; s++)
                                        putc(fd, *s);
                                break;
                        /* % */
                        case '%':
                                putc(fd, '%');
                                break;
                        case 'c':
                                putc(fd, va_arg(ap, int));
                                break;
                        /* Unknown converting symbol */
                        default:     
                                putc(fd, '%');
                                putc(fd, c);
                                break;   
                }
        }
}

void fprintf(int fd, const char * fmt, ...)
{
        va_list ap;
        
        va_start(ap, fmt);
        vprintf(fd, fmt, ap);
        va_end(ap);
}

void printf(const char *fmt, ...)
{
        va_list ap;

        va_start(ap, fmt);
        vprintf(1, fmt, ap);
        va_end(ap);
}