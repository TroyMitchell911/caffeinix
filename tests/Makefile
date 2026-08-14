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

.PHONY: qemu
qemu: printk rbtree virtqueue
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
