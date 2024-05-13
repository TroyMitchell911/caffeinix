/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-13
 * @FilePath: /caffeinix/kernel/fs/file.c
 * @Description: This file for file-system operation
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <file.h>
#include <mystring.h>
#include <debug.h>
#include <printf.h>
#include <sysfile.h>
#include <driver.h>

#define TEST_W          0
#define TEST_R          0

struct superblock sb;

struct file_table {
        struct spinlock lk;
        struct file f[NFILE];
}ftable;

/**
 * @description: File system initialization function
 * @param {uint32} dev: Which device?
 * @return {*}
 * @note This function has to be called in process context
 */
void fs_init(uint32 dev)
{
        bio_t b;
        /* Read super block */
        b = bread(dev, SUPERBLOCK_NUM);
        memmove(&sb, b->buf, sizeof(struct superblock));
        if(sb.magic != FSMAGIC)
                PANIC("fs_init");
        log_init(dev, sb.size, sb.logstart);
        brelse(b);

#if TEST_W || TEST_R
        inode_t inode;
        char buf[1024];
        int ret;
#endif

#if TEST_W
        /* The inum of first file is 2 bcs 1 is used by root dirent */
        log_begin();
        inode = iget(ROOTDEV, 2);
        ilock(inode);
        memmove(buf, "hello", 5);
        ret = writei(inode, 0, (uint64)buf, 0, 5);
        printf("%d\n", ret);
        iunlockput(inode);
        log_end();
#endif

#if TEST_R
        /* The inum of first file is 2 bcs 1 is used by root dirent */
        inode = iget(ROOTDEV, 2);
        ilock(inode);
        printf("size: %d\n", inode->d.size);
        ret = readi(inode, 0, (uint64)buf, 0, 1024);
        printf("%d\n", ret);
        for(int i = 0; i < 1024; i++) {
                printf("%c", buf[i]);
        }
        printf("\n");
        iunlockput(inode);
#endif
}

void file_init(void)
{
        spinlock_init(&ftable.lk, "ftable");
}

file_t file_alloc(void)
{
        file_t f;

        spinlock_acquire(&ftable.lk);

        for(f = ftable.f; f != &ftable.f[NFILE - 1]; f++) {
                if(f->ref == 0) {
                        f->ref = 1;
                        spinlock_release(&ftable.lk);
                        return f;
                }
        }

        spinlock_release(&ftable.lk);
        return 0;       
}

file_t file_dup(file_t f)
{
        spinlock_acquire(&ftable.lk);
        if(f->ref < 1) {
                PANIC("file_dup");
        }
        f->ref ++;
        /* 
                2024-05-12: Fixed a bug, which is that forgot release this lock
                before return the file descriptor
                By: GoKo-Son626
        */
        spinlock_release(&ftable.lk);
        return f;
}

void file_close(file_t f)
{
        struct file ff;

        spinlock_acquire(&ftable.lk);

        if(f->ref < 1) 
                PANIC("file_close");

        if(--f->ref > 0) {
                spinlock_release(&ftable.lk);
                return;
        }

        ff = *f;
        f->type = FD_NONE;
        f->ref = 0;
        /* Avoiding dead lock */
        spinlock_release(&ftable.lk);
        if(ff.type == FD_DEVICE || ff.type == FD_INODE) {
                log_begin();
                iput(ff.ip);
                log_end();
        }
}

/* addr is virtual address */
int file_read(file_t f, uint64 addr, int n)
{
        int ret = 0;

        if(!f->readable)
                return -1;

        if(f->type == FD_INODE) {
                ilock(f->ip);
                ret = readi(f->ip, 1, addr, f->off, n);
                if(ret > 0)
                        f->off += ret;
                iunlock(f->ip);
        } else if(f->type == FD_DEVICE) {
                if(f->ip->d.major < 0 || dev[f->ip->d.major].read == 0)
                        ret = -1;
                ret = dev[f->ip->d.major].read(addr, n);
        } else {
                printf("f->type = %d;", f->type);
                PANIC("file_read");
        }

        return ret;
}

/* addr is virtual address */
int file_write(file_t f, uint64 addr, int n)
{
        int ret = 0;
        
        if(!f->writable)
                return -1;

        if(f->type == FD_INODE) {
                int max = ((LOGOP-1-1-2) / 2) * BSIZE;
                int i = 0;
                /* Avoiding out of range(MAXLOGOP) */
                while(i < n) {
                        int w_n = n - i;
                        w_n = w_n > max ? max : w_n;
                        log_begin();
                        ilock(f->ip);
                        ret = writei(f->ip, 1, addr + i, f->off + i, w_n);
                        if(ret > 0)
                                f->off += ret;
                        iunlock(f->ip);
                        log_end();

                        if(ret != w_n) {
                                break;
                        }
                        i += w_n;
                }
                ret = (i == n ? n : -1);
        } else if(f->type == FD_DEVICE) {
                if(f->ip->d.major < 0 || dev[f->ip->d.major].write == 0)
                        ret = -1;
                ret = dev[f->ip->d.major].write(addr, n);
        } else {
                printf("f->type = %d;", f->type);
                PANIC("file_write");
        }

        return ret;
}