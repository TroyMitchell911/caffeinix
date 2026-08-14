ifndef CROSS_COMPILE
CROSS_COMPILE := riscv64-linux-gnu-
endif

ifndef KBUILD_RECURSIVE
.DEFAULT_GOAL := all
endif

AS		= $(CROSS_COMPILE)as
LD		= $(CROSS_COMPILE)ld
CC		= $(CROSS_COMPILE)gcc
CPP		= $(CC) -E
AR		= $(CROSS_COMPILE)ar
NM		= $(CROSS_COMPILE)nm

STRIP		= $(CROSS_COMPILE)strip
OBJCOPY		= $(CROSS_COMPILE)objcopy
OBJDUMP		= $(CROSS_COMPILE)objdump

OUTPUT = output

export AS LD CC CPP AR NM
export STRIP OBJCOPY OBJDUMP


CFLAGS = -Wall -Werror -O -std=gnu17 -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += -march=rv64gc -mabi=lp64d -mcmodel=medany
CFLAGS += -ffreestanding -fno-builtin -fno-common -nostdlib -mno-relax
CFLAGS += -nostdinc -isystem $(shell $(CC) -print-file-name=include)
CFLAGS += -fno-stack-protector -fno-pie
CFLAGS += -I.
ifeq ($(STACK_OVERFLOW_TEST),1)
CFLAGS += -DCONFIG_STACK_OVERFLOW_TEST
endif
ifeq ($(PAGE_POISONING),1)
CFLAGS += -DCONFIG_PAGE_POISONING
endif

LDFLAGS = -z max-page-size=4096
export CFLAGS LDFLAGS

TOPDIR := $(shell pwd)
export TOPDIR

# Define the subdirectory to be searched for 
# variable records (the subdirectory must contain a makefile)
obj-y += arch/riscv/boot/
obj-y += drivers/
obj-y += net/
obj-y += kernel/fs/
obj-y += kernel/
obj-y += arch/riscv/

TARGET := $(OUTPUT)/kernel

build:
	bear -- $(MAKE) all

all : $(TARGET)
	@echo $(TARGET) has been built!

.PHONY: check-uapi
check-uapi:
	$(CC) -std=gnu17 -fsyntax-only -I arch/riscv/include \
		-I kernel/include tests/linux_uapi.c

.PHONY: check-opensbi
check-opensbi: all
	CROSS_COMPILE="$(CROSS_COMPILE)" \
		tests/scripts/check-opensbi.sh

.PHONY: start_recursive_build
start_recursive_build:
	$(MAKE) -C ./ -f $(TOPDIR)/Makefile.build

ifndef KBUILD_RECURSIVE
.PHONY: built-in.o
built-in.o: start_recursive_build
endif

$(TARGET) : built-in.o kernel/kernel.ld
	@if [ ! -d $(OUTPUT) ]; then \
        	mkdir $(OUTPUT); \
    	fi
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $(TARGET) built-in.o
	$(OBJDUMP) -S $(TARGET) > $(TARGET).asm
	$(OBJDUMP) -t $(TARGET) | \
		sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(TARGET).sym

QEMU ?= qemu-system-riscv64
SBI_FIRMWARE ?= default
FS_IMG ?=
FAT_IMG ?=
ROOT_BUS ?= virtio-mmio-bus.0
FAT_BUS ?= virtio-mmio-bus.1
NET_BACKEND ?= user
NET_OPTIONS ?=
NET_BUS ?= virtio-mmio-bus.2
NET_MAC ?= 52:54:00:12:34:56
MEMORY ?= 256M
QEMU_SNAPSHOT ?=
QEMU_EXTRA_OPTS ?=
QEMU_PREBUILT ?=
comma := ,
QEMUOPTS = -machine virt -bios $(SBI_FIRMWARE) -kernel $(TARGET)
QEMUOPTS += -m $(MEMORY) -smp $(CPUS) -nographic
ifneq ($(strip $(QEMU_SNAPSHOT)),)
QEMUOPTS += -snapshot
endif
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=$(FS_IMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=$(ROOT_BUS)
ifneq ($(strip $(FAT_IMG)),)
QEMUOPTS += -drive file=$(FAT_IMG),if=none,format=raw,id=x1
QEMUOPTS += -device virtio-blk-device,drive=x1,bus=$(FAT_BUS)
endif
ifneq ($(strip $(NET_BACKEND)),)
QEMUOPTS += -netdev $(NET_BACKEND),id=n0$(if \
	$(strip $(NET_OPTIONS)),$(comma)$(NET_OPTIONS))
QEMUOPTS += -device virtio-net-device,netdev=n0,bus=$(NET_BUS),mac=$(NET_MAC)
endif
QEMUOPTS += $(QEMU_EXTRA_OPTS)
ifndef CPUS
CPUS := 8
endif
# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

.PHONY: check-fs-img
check-fs-img:
	@test -n "$(FS_IMG)" || { \
		echo "FS_IMG is required"; exit 1; \
	}
	@test -f "$(FS_IMG)" || { \
		echo "missing filesystem image: $(FS_IMG)"; exit 1; \
	}
	@if [ -n "$(FAT_IMG)" ] && [ ! -f "$(FAT_IMG)" ]; then \
		echo "missing FAT image: $(FAT_IMG)"; exit 1; \
	fi

.PHONY: qemu
ifeq ($(strip $(QEMU_PREBUILT)),)
qemu: all
endif
qemu: check-fs-img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	@sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: all .gdbinit check-fs-img
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)
	@echo "*** Now run 'gdb' in another window." 1>&2

clean:
	@find arch drivers kernel net -type f \( -name "*.o" -o \
		-name "*.asm" -o \
		-name "*.sym" -o -name "*.d" \) -delete
	@find . -maxdepth 1 -type f \( -name "*.o" -o \
		-name "*.asm" -o -name "*.sym" -o -name "*.d" \) -delete
	@if [ -d output ]; then \
		find output -maxdepth 1 -type f -delete; \
	fi
	@if [ -d .cache ]; then find .cache -mindepth 1 -delete; fi

distclean: clean
	@rm -f compile_commands.json
