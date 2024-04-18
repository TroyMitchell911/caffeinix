#include <printf.h>
#include <stdarg.h>
#include <spinlock.h>
#include <debug.h>

volatile uint8 paniced = 0;

static const char digits[] = "0123456789abcdef";
static struct {
        struct spinlock lock;
        /* This flag for panic */
        uint8 locking;
}pf;

/* Print a integer */
static void print_int(int number, uint8 base, uint8 sign)
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
                console_putc(buf[i]);
}

/* Print a pointer */
static void print_ptr(uint64 ptr)
{
        int i;
        console_putc('0');
        console_putc('x');
        /* Output every 4 bits as a number */
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
                /* We directly output if no converting symbol */
                if(c != '%'){
                        console_putc(c);
                        continue;
                }
                /* Get next character if converting symbol */
                c = fmt[++i] & 0xff;
                switch(c) {
                        /* Dec */
                        case 'd':
                                print_int(va_arg(ap, int), 10, 1);
                                break;
                        /* Hex */
                        case 'x':
                                print_int(va_arg(ap, int), 16, 1);
                                break;
                        /* Pointer */
                        case 'p':
                                print_ptr(va_arg(ap, uint64));
                                break;
                        /* String */
                        case 's':
                                if((s = va_arg(ap, char*)) == 0)
                                s = "(null)";
                                for(; *s; s++)
                                        console_putc(*s);
                                break;
                        /* % */
                        case '%':
                                console_putc('%');
                                break;
                        /* Unknown converting symbol */
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

void panic(char* s)
{
        pf.locking = 0;
        printf("[PANIC]: %s\n", s);
        paniced = 1;
        for(;;);
}

void printf_init(void)
{
        spinlock_init(&pf.lock, "printf");
        /* No panic */
        pf.locking = 1;
}