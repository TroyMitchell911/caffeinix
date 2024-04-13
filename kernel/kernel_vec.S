.global timer_vec
.align 4
timer_vec:
        csrrw x1, mscratch, x1
        # Push
        sd x2, 0(x1)
        sd x3, 8(x1)
        sd x4, 16(x1)

        # Load the address of CLINT_MTIMECMP into x2 and x3
        ld x2, 24(x1)
        addi x3, x2, 0
        # Load the value of interval into x3
        ld x4, 32(x1)
        
        # Load the value of CLINT_MTIMECMP into x2
        ld x2, 0(x3)
        # Add the interval to the CLINT_MTIMECMP value and write it to x4
        add x4, x2, x4
        # Write new value of CLINT_MTIMECMP into CLINT_MTIMECMP
        sd x4, 0(x3)

        # Pop
        ld x2, 0(x1)
        ld x3, 8(x1)
        ld x4, 16(x1)

        csrrw x1, mscratch, x1
        mret