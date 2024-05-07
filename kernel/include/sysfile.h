/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-07
 * @FilePath: /caffeinix/kernel/include/sysfile.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_SYS_FILE_H
#define __CAFFEINIX_KERNEL_SYS_FILE_H

#include <typedefs.h>
#include <myfcntl.h>

uint64 sys_open(const char* name, int flag);

uint64 sys_read(int fd, uint64 dst, int n);
uint64 sys_close(int fd);

#endif