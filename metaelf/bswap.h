/*
 * Phoenix-RTOS
 *
 * byteswap functions wrapper
 * native (compiler) or system (host) provider
 *
 * Copyright 2022 Phoenix Systems
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef BSWAP_WRAPPER_H
#define BSWAP_WRAPPER_H


/* GNUC, Clang, ICC */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* Compiler provides the best optimized */
#if __has_builtin(__builtin_bswap16) && \
	__has_builtin(__builtin_bswap32) && \
	__has_builtin(__builtin_bswap64)

/* Only in GCC */
#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)

/* System fallback */
#elif defined(__OpenBSD__)

#include <sys/types.h>
#define bswap_16(x) swap16(x)
#define bswap_32(x) swap32(x)
#define bswap_64(x) swap64(x)

#elif defined(__FreeBSD__)

#include <sys/endian.h>
#define bswap_16(x) bswap16(x)
#define bswap_32(x) bswap32(x)
#define bswap_64(x) bswap64(x)

#elif defined(__NetBSD__)

#include <sys/types.h>
#include <machine/bswap.h>
#if defined(__BSWAP_RENAME) && !defined(__bswap_32)
#define bswap_16(x) bswap16(x)
#define bswap_32(x) bswap32(x)
#define bswap_64(x) bswap64(x)

#elif defined(__sun) || defined(sun)

/* Solaris, Illumos */
#include <sys/byteorder.h>
#define bswap_16(x) BSWAP_16(x)
#define bswap_32(x) BSWAP_32(x)
#define bswap_64(x) BSWAP_64(x)

#elif defined(__APPLE__)

/* Mac OS X - Darwin */
#include <libkern/OSByteOrder.h>
#define bswap_16(x) OSSwapInt16(x)
#define bswap_32(x) OSSwapInt32(x)
#define bswap_64(x) OSSwapInt64(x)

#endif

#else

/* Linux */
#include <byteswap.h>

#endif


#endif /* end of BSWAP_WRAPPER_H */
