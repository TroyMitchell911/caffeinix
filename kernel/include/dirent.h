/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-11
 * @FilePath: /caffeinix/kernel/include/dirent.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_DIRENT_H
#define __CAFFEINIX_KERNEL_DIRENT_H

#include <inode.h>

inode_t dirlookup(inode_t ip, char *name, uint32 *poff);
int dirlink(struct inode *dp, char *name, uint32 inum);

inode_t namei(char *path);
inode_t nameiparent(char *path, char *name);

#endif