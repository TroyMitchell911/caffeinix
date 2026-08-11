#ifndef __CAFFEINIX_KERNEL_FATFS_H
#define __CAFFEINIX_KERNEL_FATFS_H

#include <block_device.h>

void fatfs_init(void);
void fatfs_set_block_device(struct block_device *device);

#endif
