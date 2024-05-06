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

typedef struct dinode {
        short type;               // File type
        short major;                    // Major device number (T_DEVICE only)
        short minor;                    // Minor device number (T_DEVICE only)
        short nlink;                    // Number of links to inode in file system
        uint32 size;                    // Size of file (bytes)
        uint32 addrs[NDIRECT+1];        // Data block addresses
}*dinode_t;

typedef struct inode {
        uint32 dev;                     // Device number
        uint32 inum;                    // Inode number

        int ref;                        // Reference count
        struct sleeplock lock;          // protects everything below here
        int valid;                      // inode has been read from disk?

        // copy of disk inode
        struct dinode d;
}*inode_t;

typedef struct dirent {
        /* It means the dirent is free if the inum equals 0 */
        unsigned short inum;
        char name[DIRSIZ];
}*dirent_t;

void iinit(void);
void iupdate(inode_t ip);
inode_t idup(inode_t ip);
uint32 imap(inode_t ip, uint32 vblock);
void itrunc(inode_t ip);
inode_t iget(uint32 dev, uint32 inum);
void iput(inode_t ip);
inode_t ialloc(uint32 dev, short type);
void ilock(inode_t ip);
void iunlock(inode_t ip);
void iunlockput(inode_t ip);
int readi(inode_t ip, int user_dst, uint64 dst, uint32 off, uint32 n);
int writei(inode_t ip, int user_src, uint64 src, uint32 off, uint32 n);

#endif