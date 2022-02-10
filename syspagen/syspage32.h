/*
 * Phoenix-RTOS
 *
 * Syspage structure for 32 bit architecture
 *
 * Copyright 2022 Phoenix Systems
 * Authors: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PHOENIX_SYSPAGE32_H_
#define _PHOENIX_SYSPAGE32_H_

#include <stdint.h>


typedef uint32_t addr32_t;
typedef uint32_t sysptr32_t;
typedef uint32_t syssize32_t;

typedef struct {
	uint32_t imgsz;
} __attribute__((packed)) hal_syspage32_t;


typedef struct _mapent_t {
	sysptr32_t next, prev;
	int32_t type;

	addr32_t start;
	addr32_t end;
} __attribute__((packed)) mapent32_t;


typedef struct _syspage_prog32_t {
	sysptr32_t next, prev;

	addr32_t start;
	addr32_t end;

	sysptr32_t argv;

	syssize32_t imapSz;
	sysptr32_t imaps;

	syssize32_t dmapSz;
	sysptr32_t dmaps;
} __attribute__((packed)) syspage_prog32_t;


typedef struct _syspage_map32_t {
	sysptr32_t next, prev;

	sysptr32_t entries;

	addr32_t start;
	addr32_t end;

	uint32_t attr;
	uint8_t id;

	sysptr32_t name;
} __attribute__((packed)) syspage_map32_t;


typedef struct {
	hal_syspage32_t hs; /* Specific syspage structure defines per architecture */
	syssize32_t size;   /* Syspage size                                        */

	addr32_t pkernel; /* Physical address of kernel's beginning */

	sysptr32_t maps;  /* Maps list    */
	sysptr32_t progs; /* Programs list*/

	uint32_t console; /* Console ID defines in hal */
} __attribute__((packed)) syspage32_t;

#endif
