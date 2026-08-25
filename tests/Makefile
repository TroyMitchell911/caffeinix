TOPDIR := $(abspath $(CURDIR)/..)
TEST_OUTPUT ?= $(TOPDIR)/output/tests
JOBS ?= $(shell nproc)

.DEFAULT_GOAL := uapi

.PHONY: uapi
uapi:
	$(MAKE) -C $(TOPDIR) check-uapi

.PHONY: opensbi
opensbi:
	$(MAKE) -C $(TOPDIR) check-opensbi

.PHONY: virtqueue
virtqueue:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/virtqueue.c $(TOPDIR)/drivers/virtio/ring.c \
		-o $(TEST_OUTPUT)/virtqueue
	$(TEST_OUTPUT)/virtqueue

.PHONY: rbtree
rbtree:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/rbtree.c $(TOPDIR)/kernel/rbtree.c \
		-o $(TEST_OUTPUT)/rbtree
	$(TEST_OUTPUT)/rbtree

.PHONY: vma
vma:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/vma.c $(TOPDIR)/kernel/vma.c \
		-o $(TEST_OUTPUT)/vma
	$(TEST_OUTPUT)/vma

.PHONY: elf
elf:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/elf.c $(TOPDIR)/kernel/elf.c \
		-o $(TEST_OUTPUT)/elf
	$(TEST_OUTPUT)/elf

.PHONY: memrange
memrange:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/memrange.c $(TOPDIR)/kernel/memrange.c \
		-o $(TEST_OUTPUT)/memrange
	$(TEST_OUTPUT)/memrange

.PHONY: buddy
buddy:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/buddy.c $(TOPDIR)/kernel/buddy.c \
		-o $(TEST_OUTPUT)/buddy
	$(TEST_OUTPUT)/buddy

.PHONY: sv39
sv39:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/sv39.c $(TOPDIR)/arch/riscv/vm_layout.c \
		-o $(TEST_OUTPUT)/sv39
	$(TEST_OUTPUT)/sv39

.PHONY: printk
printk:
	mkdir -p $(TEST_OUTPUT)
	$(CC) -std=gnu17 -Wall -Wextra -Werror -fno-builtin \
		-I $(TOPDIR)/kernel/include \
		-I $(TOPDIR)/arch/riscv/include \
		$(CURDIR)/printk.c $(TOPDIR)/kernel/printf.c \
		$(TOPDIR)/kernel/printk.c $(TOPDIR)/kernel/timeconv.c \
		-o $(TEST_OUTPUT)/printk
	$(TEST_OUTPUT)/printk

.PHONY: qemu-args
qemu-args:
	$(CURDIR)/scripts/check-qemu-args.sh

.PHONY: user-trapframe
user-trapframe:
	$(MAKE) -C $(TOPDIR) -j$(JOBS)
	$(CURDIR)/scripts/check-user-trapframe.sh

.PHONY: qemu
qemu: printk memrange buddy sv39 rbtree vma elf virtqueue qemu-args \
	user-trapframe
	TEST_OUTPUT="$(TEST_OUTPUT)" JOBS="$(JOBS)" \
		$(CURDIR)/scripts/run-qemu.sh

.PHONY: scheduler-perf
scheduler-perf:
	$(MAKE) -C $(TOPDIR) -j$(JOBS)
	TEST_OUTPUT="$(TEST_OUTPUT)" JOBS="$(JOBS)" \
		$(CURDIR)/scripts/build-rootfs.sh
	KERNEL="$(TOPDIR)/output/kernel" \
	ROOT_IMAGE="$(TEST_OUTPUT)/root.ext4" \
		$(CURDIR)/scripts/benchmark-scheduler.py \
		--output "$(TEST_OUTPUT)/scheduler-benchmark.json" --check

.PHONY: memory-perf
memory-perf:
	$(MAKE) -C $(TOPDIR) -j$(JOBS)
	TEST_OUTPUT="$(TEST_OUTPUT)" JOBS="$(JOBS)" \
		$(CURDIR)/scripts/build-rootfs.sh
	KERNEL="$(TOPDIR)/output/kernel" \
	ROOT_IMAGE="$(TEST_OUTPUT)/root.ext4" \
		$(CURDIR)/scripts/benchmark-memory.py \
		--output "$(TEST_OUTPUT)/memory-benchmark.json" --check
