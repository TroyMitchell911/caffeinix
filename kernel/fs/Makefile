EXTRA_CFLAGS := -I ../include -I ../../arch/riscv/include -I ../../include
EXTRA_LDFLAGS := -T ../kernel.ld
CFLAGS_file.o := 


obj-y += block_device.o virtio_disk.o file.o
obj-y += vfs.o char_device.o device.o devfs.o tmpfs.o
obj-y += lwext4/
obj-y += fatfs/
