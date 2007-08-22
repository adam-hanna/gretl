/* 
 *  gretl -- Gnu Regression, Econometrics and Time-series Library
 *  Copyright (C) 2001 Allin Cottrell and Riccardo "Jack" Lucchetti
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#ifndef SWAP_BYTES_H
#define SWAP_BYTES_H

#ifdef HAVE_BYTESWAP_H  /* use GNU bswap macros */

#include <byteswap.h>

#define swap_bytes_16(from, to) do { (to) = bswap_16(from); } while (0)

#define swap_bytes_32(from, to) do { (to) = bswap_32(from); } while (0)

#if defined __GNUC__ && __GNUC__ >= 2

#define swap_bytes_double(from, to)		\
do {						\
    union {					\
	unsigned long long int u64;		\
	double d;				\
    } __from, __to;				\
    __from.d = (from);				\
    __to.u64 = bswap_64(__from.u64);		\
    (to) = __to.d;				\
} while (0)

#else

#define swap_bytes_double(from, to)		\
do {						\
    union {					\
	unsigned int u32[2];			\
	double d;				\
    } __from, __to;				\
    __from.d = (from);  			\
    swap_bytes_32(__from.u32[1], __to.u32[0]);	\
    swap_bytes_32(__from.u32[0], __to.u32[1]);	\
    (to) = __to.d;				\
} while (0)

#endif

#else /* use reasonably portable definitions */

#define swap_bytes_16(from, to)						\
do {									\
    unsigned short __from16 = (from);					\
    (to) = ((((__from16) >> 8) & 0xff) | (((__from16) & 0xff) << 8));	\
} while (0)

#define swap_bytes_32(from, to)			\
do {						\
    unsigned int __from32 = (from);		\
    (to) = (((__from32 & 0xff000000) >> 24) |	\
	    ((__from32 & 0x00ff0000) >>  8) |	\
	    ((__from32 & 0x0000ff00) <<  8) |	\
	    ((__from32 & 0x000000ff) << 24));	\
} while (0)

#define swap_bytes_double(from, to)		\
do {						\
    union {					\
	unsigned int u32[2];			\
	double d;				\
    } __from, __to;				\
    __from.d = (from);  			\
    swap_bytes_32(__from.u32[1], __to.u32[0]);	\
    swap_bytes_32(__from.u32[0], __to.u32[1]);	\
    (to) = __to.d;				\
} while (0)
#endif  /* HAVE_GLIBC_BSWAP */

#define swap_bytes_ushort(from, to) swap_bytes_16(from, to)

#define reverse_ushort(x) swap_bytes_16(x, x)

#define swap_bytes_short(from, to)              \
do {						\
    union {					\
	unsigned short u16;			\
	short          s16;			\
    } __from, __to;				\
    __from.s16 = (from);			\
    swap_bytes_16(__from.u16, __to.u16);	\
    (to) = __to.s16;				\
} while (0)

#define reverse_short(x)                        \
do {						\
    union {					\
	unsigned short u16;			\
	short          s16;			\
    } __from, __to;				\
    __from.s16 = (x);    			\
    swap_bytes_16(__from.u16, __to.u16);	\
    (x) = __to.s16;				\
} while (0)

#define swap_bytes_uint(from, to) swap_bytes_32(from, to)

#define reverse_uint(x) swap_bytes_32(x, x)

#define swap_bytes_int(from, to)                \
do {						\
    union {					\
	unsigned int u32;			\
	int          s32;			\
    } __from, __to;				\
    __from.s32 = (from);			\
    swap_bytes_32(__from.u32, __to.u32);	\
    (to) = __to.s32;				\
} while (0)

#define reverse_int(x)                          \
do {						\
    union {					\
	unsigned int u32;			\
	int          s32;			\
    } __from, __to;				\
    __from.s32 = (x);    			\
    swap_bytes_32(__from.u32, __to.u32);	\
    (x) = __to.s32;				\
} while (0)

#define swap_bytes_float(from, to)		\
do {						\
    union {					\
	unsigned int u32;			\
	float f;				\
    } __from, __to;				\
    __from.f = (from);				\
    swap_bytes_32(__from.u32, __to.u32);	\
    (to) = __to.f;				\
} while (0)

#define reverse_float(x)        		\
do {						\
    union {					\
	unsigned int u32;			\
	float f;				\
    } __from, __to;				\
    __from.f = (x);				\
    swap_bytes_32(__from.u32, __to.u32);	\
    (x) = __to.f;				\
} while (0)

#define reverse_double(x) swap_bytes_double(x, x)

#endif /* SWAP_BYTES_H */
