/*
 * @Author: TroyMitchell
 * @Date: 2024-04-25
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-30
 * @FilePath: /caffeinix/kernel/include/syscall.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_SYSCALL_H
#define __CAFFEINIX_KERNEL_SYSCALL_H

#ifndef __ASSEMBLER__
#include <typedefs.h>
#endif

#ifndef __ASSEMBLER__
int fetch_str_from_user(uint64 user_addr, char* buf, int max);
int fetch_addr_from_user(uint64 user_addr, uint64* dst);
void argint(int n, int *ip);
void argaddr(int n, uint64 *ap);
int argstr(int n, char *buf, int max);
#endif

#endif
