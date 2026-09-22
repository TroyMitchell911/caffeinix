/* FatFs-backed filesystem registration and block-device selection API. */
#ifndef __CAFFEINIX_KERNEL_FATFS_H
#define __CAFFEINIX_KERNEL_FATFS_H

#include <block_device.h>

/**
 * fatfs_init() - Register the FatFs-backed filesystem type
 *
 * Context: Boot context, after VFS and block-device initialization.
 */
void fatfs_init(void);

/**
 * fatfs_set_block_device() - Select the sole FatFs adapter backing device
 * @device: Registered block device, or NULL to make the volume unavailable.
 *
 * Context: Boot or mount-setup context; no FatFs operation may be in flight.
 */
void fatfs_set_block_device(struct block_device *device);

#endif
