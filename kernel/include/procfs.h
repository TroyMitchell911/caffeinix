/* procfs registration entry point. */
#ifndef __CAFFEINIX_KERNEL_PROCFS_H
#define __CAFFEINIX_KERNEL_PROCFS_H

/**
 * procfs_init() - Register the read-only process-information filesystem
 *
 * Context: Boot context, after VFS initialization.
 */
void procfs_init(void);

#endif
