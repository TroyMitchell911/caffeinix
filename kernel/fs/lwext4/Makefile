# Build the Caffeinix adapter and pinned lwext4 sources with one configuration.
include config.mk

EXTRA_CFLAGS := -I compat -I include
EXTRA_CFLAGS += -I ../../include -I ../../../arch/riscv/include
EXTRA_CFLAGS += -I ../../../include $(LWEXT4_CONFIG_CFLAGS)
obj-y += caffeinix.o src/
