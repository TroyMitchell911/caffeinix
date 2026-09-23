# Linux socket UAPI syscall wrappers and VFS-backed protocol-stack sockets.
EXTRA_CFLAGS := -I $(TOPDIR)/kernel/include
EXTRA_CFLAGS += -I $(TOPDIR)/arch/riscv/include -I $(TOPDIR)/include
EXTRA_CFLAGS += -I $(TOPDIR)/net/lwip/port/include
EXTRA_CFLAGS += -I $(TOPDIR)/net/lwip/upstream/src/include
obj-y += socket.o syscall.o
