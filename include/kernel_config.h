/*
 * @Author: TroyMitchell
 * @Date: 2024-04-17
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-17
 * @FilePath: /caffeinix/include/kernel_config.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_CONFIG_H
#define __CAFFEINIX_KERNEL_CONFIG_H

#define NPROC                           64

#define NTHREAD                         64
#define PROC_MAXTHREAD                  32

#define MAXNAME                         16

/* For ms */
#define TICK_INTERVAL                   1
#define IDLE_TICK_INTERVAL              100

#define WORKQUEUE_NAME                  "kworker"

#define ROOT_FILESYSTEM                 "ext4"
#define INIT_PATH                       "/bin/sh"


#endif
