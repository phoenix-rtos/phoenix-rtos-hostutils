/*
 * Phoenix-RTOS
 *
 * Syspage structure for 64 bit architecture
 *
 * Copyright 2022 Phoenix Systems
 * Authors: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PHOENIX_SYSPAGE64_H_
#define _PHOENIX_SYSPAGE64_H_

#include <stdint.h>


typedef uint64_t addr64_t;
typedef uint64_t sysptr64_t;
typedef uint64_t syssize64_t;

typedef struct {
	uint32_t imgsz;
} __attribute__((packed)) hal_syspage64_t;


typedef struct _mapent64_t {
	sysptr64_t next, prev;
	int32_t type;

	addr64_t start;
	addr64_t end;
} __attribute__((packed)) mapent64_t;


typedef struct _syspage_prog64_t {
	sysptr64_t next, prev;

	addr64_t start;
	addr64_t end;

	sysptr64_t argv;

	syssize64_t imapSz;
	sysptr64_t imaps;

	syssize64_t dmapSz;
	sysptr64_t dmaps;
} __attribute__((packed)) syspage_prog64_t;


typedef struct _syspage_map64_t {
	sysptr64_t next, prev;

	sysptr64_t entries;

	addr64_t start;
	addr64_t end;

	uint32_t attr;
	uint8_t id;

	sysptr64_t name;
} __attribute__((packed)) syspage_map64_t;


typedef struct {
	hal_syspage64_t hs; /* Specific syspage structure defines per architecture */
	syssize64_t size;   /* Syspage size                                        */

	addr64_t pkernel; /* Physical address of kernel's beginning */

	sysptr64_t maps;  /* Maps list    */
	sysptr64_t progs; /* Programs list*/

	uint32_t console; /* Console ID defines in hal */
} __attribute__((packed)) syspage64_t;

#endif
