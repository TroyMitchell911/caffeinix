include config.mk

EXTRA_CFLAGS := -I compat -I include
EXTRA_CFLAGS += -I ../../include -I ../../../arch/riscv/include
EXTRA_CFLAGS += -I ../../../include $(LWEXT4_CONFIG_CFLAGS)
EXTRA_LDFLAGS := -T ../../kernel.ld

obj-y += caffeinix.o src/
