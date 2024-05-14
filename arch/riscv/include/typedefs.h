/*
 * @Author: TroyMitchell
 * @Date: 2024-05-11
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/arch/riscv/include/typedefs.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_ARCH_RISCV_TYPEDEFS_H
#define __CAFFEINIX_ARCH_RISCV_TYPEDEFS_H

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;
typedef signed long int64;

typedef unsigned int uint;

typedef uint64 size_t;

#define NELEM(x) (sizeof(x)/sizeof((x)[0]))


#endif