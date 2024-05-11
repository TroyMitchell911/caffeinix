/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-07
 * @FilePath: /caffeinix/kernel/include/file.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_FS_FILE_H
#define __CAFFEINIX_KERNEL_FS_FILE_H

#include <typedefs.h>
#include <log.h>
#include <inode.h>
#include <fs.h>

#define MAXPATH                 128
#define MAXARG                  32
#define NOFILE                  16

typedef struct file{
        enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
        int ref;                // reference count
        char readable;
        char writable;
        struct inode *ip;       // FD_INODE and FD_DEVICE
        uint32 off;             // FD_INODE
        short major;            // FD_DEVICE
}*file_t;

void fs_init(uint32 dev);

void file_init(void);
file_t file_alloc(void);
file_t file_dup(file_t f);
void file_close(file_t f);
int file_read(file_t f, uint64 addr, int n);
int file_write(file_t f, uint64 addr, int n);


#endif