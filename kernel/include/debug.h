/*
 * Fatal diagnostics and deferred state dumps. Ordinary serial-break requests
 * are deferred to the workqueue; panic output must not depend on acquiring
 * locks that may already be held.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#ifndef __CAFFEINIX_KERNEL_DEBUG_H
#define __CAFFEINIX_KERNEL_DEBUG_H

#include <printf.h>

#define DEBUG

#ifdef DEBUG

extern void panic(char* s);
void debug_init(void);
void debug_dump_state(void);
void debug_dump_state_request(void);
#define PANIC(s) do {           \
    panic(s);                   \
} while (0)

#else

#define PANIC(s)    

#endif

#endif
