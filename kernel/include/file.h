#ifndef __CAFFEINIX_KERNEL_FILE_H
#define __CAFFEINIX_KERNEL_FILE_H

#include <vfs.h>

#define MAXPATH VFS_PATH_MAX
#define MAXARG 32
#define NOFILE 16
#define NFILE 100

typedef struct vfs_file *file_t;

void file_init(void);
file_t file_alloc(void);
file_t file_dup(file_t file);
void file_close(file_t file);
file_t file_hold(file_t file);
void file_unhold(file_t file);
int64 file_read(file_t file, int user_destination, uint64 destination,
		uint64 count, uint64 *position);
int64 file_write(file_t file, int user_source, uint64 source,
		 uint64 count, uint64 *position);
int64 file_ioctl(file_t file, uint64 request, uint64 argument);

#endif
