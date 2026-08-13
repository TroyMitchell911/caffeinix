EXTRA_CFLAGS := -I ./include -I ../arch/riscv/include -I ../include 
EXTRA_LDFLAGS := -T ./kernel.ld
CFLAGS_file.o := 


obj-y += string.o palloc.o spinlock.o ktime.o thread.o wait.o \
sleeplock.o switchto.o trampoline.o process.o\
plic.o console.o printf.o scheduler.o \
exec.o sysfile.o syscall.o sysproc.o workqueue.o main.o
obj-y += irq/
