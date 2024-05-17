/*
 * @Author: TroyMitchell
 * @Date: 2024-04-17
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-15
 * @FilePath: /caffeinix/kernel/include/debug.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_DEBUG_H
#define __CAFFEINIX_KERNEL_DEBUG_H

#include <printf.h>

#define DEBUG

#ifdef DEBUG

extern void panic(char* s);
#define PANIC(s) do {           \
    panic(s);                   \
} while (0)

#else

#define PANIC(s)    

#endif

#endif