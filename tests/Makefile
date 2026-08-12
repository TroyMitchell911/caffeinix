TOPDIR := $(abspath $(CURDIR)/..)
TEST_OUTPUT ?= $(TOPDIR)/output/tests
JOBS ?= $(shell nproc)

.DEFAULT_GOAL := uapi

.PHONY: uapi
uapi:
	$(MAKE) -C $(TOPDIR) check-uapi

.PHONY: qemu
qemu:
	TEST_OUTPUT="$(TEST_OUTPUT)" JOBS="$(JOBS)" \
		$(CURDIR)/scripts/run-qemu.sh
