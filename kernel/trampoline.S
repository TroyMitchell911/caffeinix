#include <riscv.h>
#include <mem_layout.h>

.section trampsec
.global trampoline
trampoline:
.align 4
.global user_vec
user_vec:
	csrrw a0, sscratch, a0

        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

        csrr t0, sscratch
        sd t0, 112(a0)

	fsd f0, 288(a0)
	fsd f1, 296(a0)
	fsd f2, 304(a0)
	fsd f3, 312(a0)
	fsd f4, 320(a0)
	fsd f5, 328(a0)
	fsd f6, 336(a0)
	fsd f7, 344(a0)
	fsd f8, 352(a0)
	fsd f9, 360(a0)
	fsd f10, 368(a0)
	fsd f11, 376(a0)
	fsd f12, 384(a0)
	fsd f13, 392(a0)
	fsd f14, 400(a0)
	fsd f15, 408(a0)
	fsd f16, 416(a0)
	fsd f17, 424(a0)
	fsd f18, 432(a0)
	fsd f19, 440(a0)
	fsd f20, 448(a0)
	fsd f21, 456(a0)
	fsd f22, 464(a0)
	fsd f23, 472(a0)
	fsd f24, 480(a0)
	fsd f25, 488(a0)
	fsd f26, 496(a0)
	fsd f27, 504(a0)
	fsd f28, 512(a0)
	fsd f29, 520(a0)
	fsd f30, 528(a0)
	fsd f31, 536(a0)
	frcsr t0
	sd t0, 544(a0)

        ld sp, 8(a0)

        ld tp, 32(a0)

        ld t0, 16(a0)

        ld t1, 0(a0)

        sfence.vma zero, zero
        csrw satp, t1
        sfence.vma zero, zero
        jr t0

.global user_ret
user_ret:
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

	csrw sscratch, a1
	mv a0, a1

	ld t0, 544(a0)
	fscsr t0
	fld f0, 288(a0)
	fld f1, 296(a0)
	fld f2, 304(a0)
	fld f3, 312(a0)
	fld f4, 320(a0)
	fld f5, 328(a0)
	fld f6, 336(a0)
	fld f7, 344(a0)
	fld f8, 352(a0)
	fld f9, 360(a0)
	fld f10, 368(a0)
	fld f11, 376(a0)
	fld f12, 384(a0)
	fld f13, 392(a0)
	fld f14, 400(a0)
	fld f15, 408(a0)
	fld f16, 416(a0)
	fld f17, 424(a0)
	fld f18, 432(a0)
	fld f19, 440(a0)
	fld f20, 448(a0)
	fld f21, 456(a0)
	fld f22, 464(a0)
	fld f23, 472(a0)
	fld f24, 480(a0)
	fld f25, 488(a0)
	fld f26, 496(a0)
	fld f27, 504(a0)
	fld f28, 512(a0)
	fld f29, 520(a0)
	fld f30, 528(a0)
	fld f31, 536(a0)

	# Restore all but a0 from the current thread's trapframe.
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0
        ld a0, 112(a0)
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
