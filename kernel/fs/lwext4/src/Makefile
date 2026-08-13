include ../config.mk

EXTRA_CFLAGS := -I ../compat -I ../include
EXTRA_CFLAGS += -I ../../../include -I ../../../../arch/riscv/include
EXTRA_CFLAGS += -I ../../../../include $(LWEXT4_CONFIG_CFLAGS)
obj-y += ext4.o ext4_balloc.o ext4_bcache.o ext4_bitmap.o
obj-y += ext4_block_group.o ext4_blockdev.o ext4_crc32.o
obj-y += ext4_debug.o ext4_dir.o ext4_dir_idx.o ext4_extent.o
obj-y += ext4_fs.o ext4_hash.o ext4_ialloc.o ext4_inode.o
obj-y += ext4_journal.o ext4_super.o ext4_trans.o ext4_xattr.o
