#ifndef __CAFFEINIX_KERNEL_FS_FS_H
#define __CAFFEINIX_KERNEL_FS_FS_H

#include <typedefs.h>

/* Max loaded inode */
#define NINODES                         50
/* Max opened file per system */
#define NFILE                           100
/* Max opened file per system */
#define NPFILE                          16

/* How many bits per block : 8096 bits */
#define BPB                             (1024*8)
/* How many inodes per block */
#define IPB                             (BSIZE / sizeof(struct inode))

/* Just a magic number */
#define FSMAGIC                         0x20030528
/* Which block is used by superblock */
#define SUPERBLOCK_NUM                  1
/* How many blocks file-system used */
#define FSSIZE                          2000
#define MAXFILE                         (NDIRECT + NINDIRECT)

#define NDIRECT                         12
#define NINDIRECT                       (BSIZE / sizeof(uint32))
#define DIRSIZ                          14
#define ROOTINO                         1
#define ROOTDEV                         1

#define IBLOCK(i, sb)                   ((i) / IPB + sb.inodestart)
#define BBLOCK(i, sb)                   ((i) / BPB + sb.bmapstart)

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

#endif