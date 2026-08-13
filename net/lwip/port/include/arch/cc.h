#ifndef __CAFFEINIX_LWIP_ARCH_CC_H
#define __CAFFEINIX_LWIP_ARCH_CC_H

#include <debug.h>
#include <mystring.h>
#include <printf.h>
#include <typedefs.h>

#define LWIP_NO_STDDEF_H 1
#define LWIP_NO_STDINT_H 1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1

typedef uint8 u8_t;
typedef int8 s8_t;
typedef uint16 u16_t;
typedef int16 s16_t;
typedef uint32 u32_t;
typedef int32 s32_t;
typedef uint64 u64_t;
typedef int64 s64_t;
typedef int64 ptrdiff_t;
typedef uint64 mem_ptr_t;
typedef int64 ssize_t;

int *lwip_errno_location(void);
#define errno (*lwip_errno_location())

#define LWIP_HAVE_INT64 1
#define INT_MAX 2147483647
#define SSIZE_MAX 0x7fffffffffffffffL

#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define BYTE_ORDER LITTLE_ENDIAN

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(field) field
#define PACK_STRUCT_FLD_8(field) field
#define PACK_STRUCT_FLD_S(field) field

#define LWIP_PLATFORM_DIAG(arguments) do { printf arguments; } while (0)
#define LWIP_PLATFORM_ASSERT(message) PANIC((char *)(message))

u32_t lwip_port_rand(void);
#define LWIP_RAND() lwip_port_rand()

#endif
