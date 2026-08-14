#include <mem_layout.h>

.global kernel_trap
.global kernel_stack_overflow
.global kernel_vec
.align 4
kernel_vec:
	/* Preserve t0 while checking the aligned stack slot. */
	csrw sscratch, t0

	/* Make room to save registers. */
        addi sp, sp, -256
	srli t0, sp, KSTACK_SHIFT
	andi t0, t0, 1
	bnez t0, .Lkernel_stack_overflow
	csrr t0, sscratch

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

.Lkernel_stack_overflow:
	/* Report from the per-CPU emergency stack, not the failed stack. */
	addi a0, sp, 256
	la t0, cpu_overflow_stack_tops
	ld t0, 0(t0)
	slli t1, tp, 3
	add t0, t0, t1
	ld sp, 0(t0)
	tail kernel_stack_overflow

#ifdef CONFIG_STACK_OVERFLOW_TEST
.global kernel_stack_overflow_test
kernel_stack_overflow_test:
	addi sp, sp, -2048
	sd zero, 0(sp)
	j kernel_stack_overflow_test
#endif
