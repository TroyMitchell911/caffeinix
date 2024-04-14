.global kernel_trap
.global kernel_vec
.align 4
kernel_vec:
        # make room to save registers.
        addi sp, sp, -256

        # save the registers.
        sd ra, 0(sp)
        sd sp, 8(sp)
        sd gp, 16(sp)
        sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd s0, 56(sp)
        sd s1, 64(sp)
        sd a0, 72(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd s2, 136(sp)
        sd s3, 144(sp)
        sd s4, 152(sp)
        sd s5, 160(sp)
        sd s6, 168(sp)
        sd s7, 176(sp)
        sd s8, 184(sp)
        sd s9, 192(sp)
        sd s10, 200(sp)
        sd s11, 208(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        # call the C trap handler in trap.c
        call kernel_trap

        # restore registers.
        ld ra, 0(sp)
        ld sp, 8(sp)
        ld gp, 16(sp)
        # not tp (contains hartid), in case we moved CPUs
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld s0, 56(sp)
        ld s1, 64(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld s2, 136(sp)
        ld s3, 144(sp)
        ld s4, 152(sp)
        ld s5, 160(sp)
        ld s6, 168(sp)
        ld s7, 176(sp)
        ld s8, 184(sp)
        ld s9, 192(sp)
        ld s10, 200(sp)
        ld s11, 208(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        # restore sp
        addi sp, sp, 256

        # return to whatever we were doing in the kernel.
        sret        


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

        # Arrange for a supervisor software interrupt
        # after this handler returns
        li x2, 2
        csrw sip, x2

        # Pop
        ld x2, 0(x1)
        ld x3, 8(x1)
        ld x4, 16(x1)

        csrrw x1, mscratch, x1
        mret