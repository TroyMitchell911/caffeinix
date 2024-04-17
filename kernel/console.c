#include <console.h>
#include <spinlock.h>

struct {
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

static struct spinlock lock;

#define BACKSPACE       0x100
#define C(x)            ((x)-'@')  // Control-x

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

void console_init(void)
{
        spinlock_init(&lock, "console");
        uart_init();
}