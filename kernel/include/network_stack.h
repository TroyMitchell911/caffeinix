#ifndef __CAFFEINIX_KERNEL_NETWORK_STACK_H
#define __CAFFEINIX_KERNEL_NETWORK_STACK_H

#include <typedefs.h>

void network_stack_init(void);
int network_stack_address_is_broadcast(uint32 address);

#endif
