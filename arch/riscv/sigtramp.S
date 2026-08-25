#define RISCV_LINUX_SYS_RT_SIGRETURN 139

.section sigtrampsec
.global sigtrampoline
sigtrampoline:
	li a7, RISCV_LINUX_SYS_RT_SIGRETURN
	ecall

.global sigtrampoline_end
sigtrampoline_end:
