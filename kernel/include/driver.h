/*
 * @Author: TroyMitchell
 * @Date: 2024-05-12
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-12
 * @FilePath: /caffeinix/kernel/include/driver.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_DRIVER_H
#define __CAFFEINIX_KERNEL_DRIVER_H

#include <typedefs.h>

#define NDEV            10

#define CONSOLE         1

typedef struct driver_opt {
        int (*write)(uint64 src, int n);
        int (*read)(uint64 dst, int n);
}*driver_opt_t;

extern struct driver_opt dev[NDEV];

#endif