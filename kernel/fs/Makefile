EXTRA_CFLAGS := -I ../include -I ../../arch/riscv/include -I ../../include
EXTRA_LDFLAGS := -T ../kernel.ld
CFLAGS_file.o := 


obj-y += block_device.o virtio_disk.o bio.o log.o balloc.o \
inode.o file.o dirent.o
obj-y += vfs.o
