#ifndef LWEXT4_COMPAT_STDDEF_H
#define LWEXT4_COMPAT_STDDEF_H

typedef unsigned long size_t;
typedef signed long ptrdiff_t;

#define NULL ((void *)0)
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif
