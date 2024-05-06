/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-06 13:25
 * @FilePath: /caffeinix/kernel/fs/file.c
 * @Description: This file for file-system operation
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <file.h>
#include <mystring.h>
#include <debug.h>
#include <printf.h>

struct superblock sb;

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

        /* The inum of first file is 2 bcs 1 is used by root dirent */
        inode_t inode = iget(ROOTDEV, 2);
        ilock(inode);
        printf("size: %d\n", inode->d.size);
        uint8 buf[1024];
        int ret = readi(inode, 0, (uint64)buf, 0, 1024);
        printf("%d\n", ret);
        for(int i = 0; i < 1024; i++) {
                printf("%c", buf[i]);
        }
        printf("\n");
        iunlockput(inode);
}