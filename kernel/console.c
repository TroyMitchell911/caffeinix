#include <console.h>
#include <spinlock.h>

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



void console_init(void)
{
        spinlock_init(&console.lock, "console");
        uart_init();
        uart_register_rx_callback(console_intr);
}