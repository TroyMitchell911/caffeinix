#include <uart.h>
#include <string.h>

#define REG(r)                  ((volatile unsigned char *)(UART0 + r))

/* receive holding register (for input bytes) */
#define RHR                     0          
// transmit holding register (for output bytes)       
#define THR                     0
/* interrupt enable register */             
#define IER                     1                 
#define IER_RX_ENABLE           (1<<0)
#define IER_TX_ENABLE           (1<<1)
/* FIFO control register */
#define FCR                     2                 
#define FCR_FIFO_ENABLE         (1<<0)
/* clear the content of the two FIFOs */
#define FCR_FIFO_CLEAR          (3<<1) 
/* interrupt status register */
#define ISR                     2        
/* line control register */         
#define LCR                     3                 
#define LCR_EIGHT_BITS          (3<<0)
/* special mode to set baud rate */
#define LCR_BAUD_LATCH          (1<<7) 
/* line status register */
#define LSR                     5    
/* input is waiting to be read from RHR */             
#define LSR_RX_READY            (1<<0)   
/* THR can accept another character to send */
#define LSR_TX_IDLE             (1<<5)    

#define REG_R(r)                (*(REG(r)))
#define REG_W(r, v)             (*(REG(r)) = (v))

void uart_init(void)
{
        /* disable interrupts. */
        REG_W(IER, 0x00);

        /* special mode to set baud rate. */
        REG_W(LCR, LCR_BAUD_LATCH);

        /* LSB for baud rate of 38.4K. */
        REG_W(0, 0x03);

        /* MSB for baud rate of 38.4K. */
        REG_W(1, 0x00);

        /* 
                leave set-baud mode,
                and set word length to 8 bits, no parity. 
        */
        REG_W(LCR, LCR_EIGHT_BITS);

        /* reset and enable FIFOs. */
        REG_W(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

        /* enable transmit and receive interrupts. */
        // REG_W(IER, IER_TX_ENABLE | IER_RX_ENABLE);
}

void uart_putc(int c)
{
        REG_W(THR, c);
}

void uart_puts(const char* str)
{
        int n;
        n = strlen(str);

        while(n --) {
                uart_putc(*str++);
        }
}