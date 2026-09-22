/*
 * Process virtual-memory lifecycle and page-fault policy.
 *
 * These helpers bridge syscall VMA metadata, RISC-V page tables, file cache,
 * and anonymous backing.  A live process is serialized by mmap_lock.
 */
#ifndef __CAFFEINIX_KERNEL_MMAP_H
#define __CAFFEINIX_KERNEL_MMAP_H

#include <typedefs.h>

struct process;
struct vfs_file;
struct vfs_inode;

enum mmap_fault_access {
	MMAP_FAULT_READ,
	MMAP_FAULT_WRITE,
	MMAP_FAULT_EXEC,
	MMAP_FAULT_POPULATE,
};

enum mmap_fault_result {
	MMAP_FAULT_OK,
	MMAP_FAULT_MAPERR,
	MMAP_FAULT_ACCERR,
	MMAP_FAULT_BUSERR,
	MMAP_FAULT_NOMEM,
	MMAP_FAULT_RETRY,
};

/**
 * mmap_handle_fault() - Resolve one user page-table fault.
 * @process: Faulting process with a live address space.
 * @address: Faulting virtual address.
 * @access: Requested read, write, execute, or prefault access.
 *
 * Context: Thread context; may allocate, do filesystem I/O, and sleep.
 * Return: A mmap_fault_result describing mapping, protection, I/O, memory,
 * or retry failure. The caller converts it to the architecture trap action.
 */
enum mmap_fault_result mmap_handle_fault(struct process *process,
					 uint64 address,
					 enum mmap_fault_access access);
/**
 * mmap_init() - Initialize the registry of processes with address spaces.
 *
 * Context: Early boot, before any process registers. Does not sleep.
 */
void mmap_init(void);
/**
 * mmap_process_register() - Add a process to the mmap reclaim registry.
 * @process: Process with separately initialized VMA and page-table state.
 *
 * Context: May take the registry sleeplock. The process must not already be
 * registered. This function does not create or destroy VMAs.
 */
void mmap_process_register(struct process *process);
/**
 * mmap_process_unregister() - Remove a process from mmap reclaim scans.
 * @process: Registered process no longer eligible for registry traversal.
 *
 * Context: May take the registry sleeplock. The caller tears down page tables
 * and VMA references separately.
 */
void mmap_process_unregister(struct process *process);
/**
 * mmap_process_fork() - Clone parent mappings into child using COW.
 * @parent: Running parent with a registered address space.
 * @child: Initialized but unpublished child with a private page directory.
 *
 * Context: May allocate and takes the registry and parent mmap locks.
 * Return: %0 or %-1; failure leaves @child ready for normal teardown.
 */
int mmap_process_fork(struct process *parent, struct process *child);
int mmap_process_vfork(struct process *parent, struct process *child);
int mmap_process_vfork_detach(struct process *parent,
			      struct process *child, uint64 *shared);
/**
 * mmap_file_truncate() - Invalidate VMA/cache state after a file resize.
 * @inode: Resized inode.
 * @old_size: Previous byte size.
 * @size: New byte size.
 *
 * Context: Thread context; takes registry and process mmap locks and may
 * yield while page-cache truncation requests retry.
 * Return: PAGE_CACHE_TRUNCATE_OK or PAGE_CACHE_TRUNCATE_ERROR. Internal retry
 * results are consumed before this function returns.
 */
int mmap_file_truncate(struct vfs_inode *inode, uint64 old_size,
		       uint64 size);
int mmap_reclaim_file_page(struct vfs_file *file, uint64 offset,
			   void *page);
/**
 * mmap_reclaim_clean_pages() - Reclaim clean pages from inactive mappings.
 * @target: Maximum pages to reclaim.
 *
 * Context: Thread context; may take process mmap locks and sleep.
 * Return: Number reclaimed.
 */
uint64 mmap_reclaim_clean_pages(uint64 target);
int mmap_process_usage(int pid, uint64 *virtual_size,
		       uint64 *resident_pages);

#endif
