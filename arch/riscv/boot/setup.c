#include <kernel_config.h>
#include <riscv.h>

extern void main(void);

/* Create a stack that the CPUS go into c environment */
__attribute__ ((aligned (16))) int8 stack_for_c[4096 * NCPU];

void setup(void)
{
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
        
        /* Allow 'Supervisor mode' to access all 128M of memory */
        pmpaddr0_w(0x3fffffffffffffull);
        pmpcfg0_W(0xf);

        /* Enter 'Supervisor mode' and 'main'  */
        asm volatile("mret");
}