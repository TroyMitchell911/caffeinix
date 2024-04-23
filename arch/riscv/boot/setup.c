#include <kernel_config.h>
#include <riscv.h>

extern void main(void);
extern void timer_init(uint8 hartid);

/* Create a stack that the CPUS go into c environment */
__attribute__ ((aligned (16))) int8 stack_for_c[4096 * NCPU];

void setup(void)
{
        int hartid;

        /* Read value of status into variable 'status' */
        uint64 status = mstatus_r();
        /* Clear the flag of previous mode */
        status &= ~MSTATUS_MPP_MASK;
        /* Set the flag of previous mode to 'Supervisor mode' */
        status |= MSTATUS_MPP_S;
        mstatus_w(status);

        /* Write return address */
        mepc_w((uint64)main);

        /* Dsiable page-table function */
        satp_w(0);

        /* Delegate all interrupts and exceptions to supervisor mode. */
        medeleg_w(0xffff);
        mideleg_w(0xffff);
        /* Enable interrupt */
        sie_w(sie_r() | SIE_SEIE | SIE_SSIE | SIE_STIE);
        
        /* Allow 'Supervisor mode' to access all 128M of memory */
        pmpaddr0_w(0x3fffffffffffffull);
        pmpcfg0_w(0xf);

        /* Initialize the timer corresponding to each hart */
        hartid = mhartid_r();
        /* Write hartid into the register 'tp' */
        tp_w(hartid);

        // timer_init(hartid);

        /* Enter 'Supervisor mode' and 'main'  */
        asm volatile("mret");
}