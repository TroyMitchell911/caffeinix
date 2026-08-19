#ifndef __CAFFEINIX_KERNEL_MMAP_H
#define __CAFFEINIX_KERNEL_MMAP_H

#include <typedefs.h>

struct process;

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

#endif
