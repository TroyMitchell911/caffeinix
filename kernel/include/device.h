#ifndef __CAFFEINIX_KERNEL_DEVICE_H
#define __CAFFEINIX_KERNEL_DEVICE_H

#include <vfs.h>

#define DEVICE_NULL VFS_MAKE_DEVICE(1, 3)
#define DEVICE_ZERO VFS_MAKE_DEVICE(1, 5)
#define DEVICE_TTY VFS_MAKE_DEVICE(5, 0)
#define DEVICE_CONSOLE VFS_MAKE_DEVICE(5, 1)

extern const struct vfs_file_operations vfs_device_operations;

#endif
