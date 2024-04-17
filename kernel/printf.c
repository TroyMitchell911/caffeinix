#include <printf.h>

static const char digits[] = "0123456789abcdef";

void print_int(int number, uint8 base, uint8 sign)
{
        char buf[16];
        int i;
        uint32 num;

        if(sign && (sign = number < 0))
                num = -number;
        else
                num = number;

        i = 0;
        do {
                buf[i++] = digits[num % base];
        } while((num /= base) != 0);

        if(sign)
                buf[i++] = '-';

        while(--i >= 0)
                console_putc(buf[i]);
}

void print_ptr(uint64 ptr)
{
        int i;
        console_putc('0');
        console_putc('x');
        for (i = 0; i < (sizeof(uint64) * 2); i++, ptr <<= 4)
                console_putc(digits[ptr >> (sizeof(uint64) * 8 - 4)]);
}
