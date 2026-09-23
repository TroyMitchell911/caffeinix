/* tmpfs registration entry point. */
#ifndef __CAFFEINIX_KERNEL_TMPFS_H
#define __CAFFEINIX_KERNEL_TMPFS_H

/**
 * tmpfs_init() - Register the in-memory filesystem type
 *
 * Context: Boot context, after VFS initialization.
 */
void tmpfs_init(void);

#endif
