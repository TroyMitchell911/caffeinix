#include <kernel_config.h>

/* Create a stack that the CPUS go into c environment */
__attribute__ ((aligned (16))) char stack_for_c[4096 * NCPU];

void setup(void) 
{
    
}