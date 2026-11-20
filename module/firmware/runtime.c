/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding byte primitives shared with the genuine Amiga FFS engine.
 * This profile does not import libc, OS filesystem services, or EFI drivers.
 */
#include <stddef.h>
void *memcpy(void *restrict dst,const void *restrict src,size_t n) {
 unsigned char *out=dst;const unsigned char *in=src;
 for(size_t i=0;i<n;++i)out[i]=in[i];
 return dst;
}
void *memset(void *dst,int ch,size_t n) {
 unsigned char *out=dst;
 for(size_t i=0;i<n;++i)out[i]=(unsigned char)ch;
 return dst;
}
