# FatFs and its local VFS/block glue use ffconf.h and freestanding compat types.
EXTRA_CFLAGS := -I compat -I . -I ../../include \
	-I ../../../arch/riscv/include -I ../../../include
obj-y += caffeinix.o diskio.o ff.o ffunicode.o
