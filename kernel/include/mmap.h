#ifndef __CAFFEINIX_KERNEL_MMAP_H
#define __CAFFEINIX_KERNEL_MMAP_H

#include <typedefs.h>

struct process;
struct vfs_inode;

enum mmap_fault_access {
	MMAP_FAULT_READ,
	MMAP_FAULT_WRITE,
	MMAP_FAULT_EXEC,
};

enum mmap_fault_result {
	MMAP_FAULT_OK,
	MMAP_FAULT_MAPERR,
	MMAP_FAULT_ACCERR,
	MMAP_FAULT_BUSERR,
	MMAP_FAULT_NOMEM,
};

enum mmap_fault_result mmap_handle_fault(struct process *process,
					 uint64 address,
					 enum mmap_fault_access access);
void mmap_init(void);
void mmap_process_register(struct process *process);
void mmap_process_unregister(struct process *process);
int mmap_process_fork(struct process *parent, struct process *child);
int mmap_file_truncate(struct vfs_inode *inode, uint64 old_size,
		       uint64 size);

#endif
