/* devfs registration entry point. */
#ifndef __CAFFEINIX_KERNEL_DEVFS_H
#define __CAFFEINIX_KERNEL_DEVFS_H

/**
 * devfs_init() - Register the synthetic device filesystem type
 *
 * Context: Boot context, after VFS initialization and before mounting /dev.
 */
void devfs_init(void);

#endif
