/* lwext4-backed filesystem registration and diagnostic interfaces. */
#ifndef __CAFFEINIX_KERNEL_EXT4FS_H
#define __CAFFEINIX_KERNEL_EXT4FS_H

/**
 * ext4fs_init() - Register the lwext4-backed filesystem type
 *
 * Context: Boot context, after VFS and block-device initialization.
 */
void ext4fs_init(void);

/**
 * ext4fs_debug_dump() - Print the current adapter state for diagnostics
 *
 * Context: Thread context; output is best-effort and does not alter state.
 */
void ext4fs_debug_dump(void);

#endif
