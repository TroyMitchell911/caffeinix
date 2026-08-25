.text
.global signal_register_probe
.type signal_register_probe, @function
signal_register_probe:
	addi sp, sp, -48
	sd ra, 40(sp)
	sd s2, 32(sp)
	fsd fs0, 24(sp)
	sd a0, 16(sp)
	sd a1, 8(sp)

	li s2, 0x123
	li t0, 0x3ff8000000000000
	fmv.d.x fs0, t0
	li a0, 12
	call raise
	mv t2, a0
	bnez a0, .Lsignal_register_done
	ld t0, 16(sp)
	sd s2, 0(t0)
	fmv.x.d t1, fs0
	ld t0, 8(sp)
	sd t1, 0(t0)

.Lsignal_register_done:
	ld ra, 40(sp)
	ld s2, 32(sp)
	fld fs0, 24(sp)
	addi sp, sp, 48
	mv a0, t2
	ret
.size signal_register_probe, .-signal_register_probe

.global signal_a0_probe
.type signal_a0_probe, @function
signal_a0_probe:
	li a7, 131
	ecall
	ret
.size signal_a0_probe, .-signal_a0_probe
