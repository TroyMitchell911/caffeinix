/*
 * Fixed kernel resource limits and boot defaults. CPU topology and usable RAM
 * come from firmware, not these limits. Tick constants are milliseconds;
 * INIT_PATH is resolved after the root filesystem is mounted.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#ifndef __CAFFEINIX_KERNEL_CONFIG_H
#define __CAFFEINIX_KERNEL_CONFIG_H

#define NPROC                           64

#define NTHREAD                         64
#define PROC_MAXTHREAD                  32

#define MAXNAME                         16

/* Active and idle per-hart timer periods in milliseconds. */
#define TICK_INTERVAL                   1
#define IDLE_TICK_INTERVAL              100

#define WORKQUEUE_NAME                  "kworker"

#define ROOT_FILESYSTEM                 "ext4"
#define INIT_PATH                       "/bin/sh"


#endif
