#include <printf.h>
#include <stdarg.h>
#include <spinlock.h>
#include <debug.h>

static const char digits[] = "0123456789abcdef";
static struct {
        struct spinlock lock;
        uint8 locking;
}pf;

static void print_int(int number, uint8 base, uint8 sign)
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

static void print_ptr(uint64 ptr)
{
        int i;
        console_putc('0');
        console_putc('x');
        for (i = 0; i < (sizeof(uint64) * 2); i++, ptr <<= 4)
                console_putc(digits[ptr >> (sizeof(uint64) * 8 - 4)]);
}

void printf(char* fmt, ...)
{
        va_list ap;
        int i, c, locking;
        char *s;

        locking = pf.locking;
        if(locking)
                spinlock_acquire(&pf.lock);

        if(fmt == 0)
                PANIC("printf");

        va_start(ap, fmt);
        for(i = 0; (c = fmt[i] & 0xff) != 0; i++) {
                if(c != '%'){
                        console_putc(c);
                        continue;
                }
                c = fmt[++i] & 0xff;
                switch(c) {
                        case 'd':
                                print_int(va_arg(ap, int), 10, 1);
                                break;
                        case 'x':
                                print_int(va_arg(ap, int), 16, 1);
                                break;
                        case 'p':
                                print_ptr(va_arg(ap, uint64));
                                break;
                        case 's':
                                if((s = va_arg(ap, char*)) == 0)
                                s = "(null)";
                                for(; *s; s++)
                                        console_putc(*s);
                                break;
                        case '%':
                                console_putc('%');
                                break;
                        default:     
                                console_putc('%');
                                console_putc(c);
                                break;   
                }
        }
        va_end(ap);

        if(locking)
                spinlock_release(&pf.lock);
}

void printf_init(void)
{
        spinlock_init(&pf.lock, "printf");
        pf.locking = 1;
}