ifndef CROSS_COMPILE
CROSS_COMPILE := riscv64-caffeinix-
endif

AS		= $(CROSS_COMPILE)gas
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


CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096
export CFLAGS LDFLAGS

TOPDIR := $(shell pwd)
export TOPDIR

# Define the subdirectory to be searched for 
# variable records (the subdirectory must contain a makefile)
obj-y += arch/riscv/boot/
obj-y += kernel/fs/
obj-y += kernel/
obj-y += arch/riscv/

TARGET := $(OUTPUT)/kernel

build:
	bear -- make all

all : start_recursive_build $(TARGET)
	@echo $(TARGET) has been built!

start_recursive_build:
	make -C ./ -f $(TOPDIR)/Makefile.build

$(TARGET) : built-in.o
	@if [ ! -d $(OUTPUT) ]; then \
        	mkdir $(OUTPUT); \
    	fi
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $(TARGET) built-in.o
	$(OBJDUMP) -S $(TARGET) > $(TARGET).asm
	$(OBJDUMP) -t $(TARGET) | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(TARGET).sym

QEMU = qemu-system-riscv64
QEMUOPTS = -machine virt -bios none -kernel $(TARGET) -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=./fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
ifndef CPUS
CPUS := 1
endif
# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

qemu: all
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	@sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: all .gdbinit
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)
	@echo "*** Now run 'gdb' in another window." 1>&2

clean:
	@rm -f $(shell find -name "*.o")
	@rm -f $(shell find -name "*.asm")
	@rm -f $(shell find -name "*.sym")
	@rm -f $(shell find -name "*.d")
	@rm -f output/*
	@rm -rf .cache/*

distclean: clean
	@rm -f compile_commands.json
