/* Caffeinix freestanding type-format shim required by imported lwext4. */
#ifndef LWEXT4_COMPAT_INTTYPES_H
#define LWEXT4_COMPAT_INTTYPES_H

#include <stdint.h>

#define PRIu64 "lu"
#define PRId64 "ld"
#define PRIx64 "lx"
#define PRIX64 "lX"

#endif
