#ifndef __CAFFEINIX_KERNEL_RANDOM_H
#define __CAFFEINIX_KERNEL_RANDOM_H

#include <typedefs.h>

void random_init(void);
void random_add_hardware(const void *buffer, uint32 length);
void random_finalize_boot(void);
int get_random_bytes(void *buffer, uint64 length);
int get_random_u64(uint64 *value);
int random_is_strong(void);

#endif
