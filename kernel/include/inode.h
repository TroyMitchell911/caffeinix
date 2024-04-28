#ifndef __CAFFEINIX_KERNEL_FS_INODE_H
#define __CAFFEINIX_KERNEL_FS_INODE_H

#include <typedefs.h>
#include <sleeplock.h>
#include <log.h>
#include <fs.h>

typedef enum file_type {
        T_DIR = 1,
        T_FILE,
        T_DEVICE
}file_type_t;

struct dinode {
        short type;                     // File type
        short major;                    // Major device number (T_DEVICE only)
        short minor;                    // Minor device number (T_DEVICE only)
        short nlink;                    // Number of links to inode in file system
        uint32 size;                    // Size of file (bytes)
        uint32 addrs[NDIRECT+1];        // Data block addresses
};

struct inode {
        uint32 dev;                     // Device number
        uint32 inum;                    // Inode number

        int ref;                        // Reference count
        struct sleeplock lock;          // protects everything below here
        int valid;                      // inode has been read from disk?

        short type;                     // copy of disk inode
        short major;
        short minor;
        short nlink;
        uint32 size;
        uint32 addrs[NDIRECT+1];
};

struct dirent {
// 为0代表该目录项空闲
  unsigned short inum;
  char name[DIRSIZ];
};

#endif