EXTRA_CFLAGS := -I ./include -I ../arch/riscv/include -I ../include 
EXTRA_LDFLAGS := -T ./kernel.ld
CFLAGS_file.o := 


obj-y += string.o palloc.o thread.o spinlock.o \
sleeplock.o switchto.o trampoline.o process.o\
plic.o console.o printf.o scheduler.o \
exec.o sysfile.o syscall.o driver.o main.o 