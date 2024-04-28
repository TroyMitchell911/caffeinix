#ifndef __CAFFEINIX_KERNEL_FS_FILE_H
#define __CAFFEINIX_KERNEL_FS_FILE_H

#include <typedefs.h>
#include <log.h>

#define FSMAGIC                 0x10203040
#define SUPERBLOCK_NUM          1

typedef struct superblock {
        uint32 magic;        // Must be FSMAGIC
        uint32 size;         // Size of file system image (blocks)
        uint32 nblocks;      // Number of data blocks
        uint32 ninodes;      // Number of inodes.
        uint32 nlog;         // Number of log blocks
        uint32 logstart;     // Block number of first log block
        uint32 inodestart;   // Block number of first inode block
        uint32 bmapstart;    // Block number of first free map block
}*superblock_t;

void fs_init(uint32 dev);

#endif