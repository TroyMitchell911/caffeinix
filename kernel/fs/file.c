/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-06 14:02
 * @FilePath: /caffeinix/kernel/fs/file.c
 * @Description: This file for file-system operation
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <file.h>
#include <mystring.h>
#include <debug.h>
#include <printf.h>

#define TEST_W          1
#define TEST_R          1

struct superblock sb;

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
        uint8 buf[1024];
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