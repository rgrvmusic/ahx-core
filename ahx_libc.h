/* SPDX-License-Identifier: MIT */
/* Use the host string library when available; otherwise declare the firmware's libc functions. */
#ifndef AHX_LIBC_H
#define AHX_LIBC_H

#if defined(__has_include)
#if __has_include(<string.h>)
#include <string.h>
#define AHX_HAVE_STRING_H 1
#endif
#endif

#ifndef AHX_HAVE_STRING_H
/* the device: felucca/firmware/src/libc.c */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#endif

#endif /* AHX_LIBC_H */
