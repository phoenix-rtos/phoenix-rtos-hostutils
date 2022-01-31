/*
 * Phoenix-RTOS
 *
 * Syspage structure
 *
 * Copyright 2022 Phoenix Systems
 * Authors: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _PHOENIX_SYSPAGE_H_
#define _PHOENIX_SYSPAGE_H_

#include <stdint.h>

enum { mAttrRead = 0x01, mAttrWrite = 0x02, mAttrExec = 0x04, mAttrShareable = 0x08,
	   mAttrCacheable = 0x10, mAttrBufferable = 0x20 };


enum { console_default = 0, console_com0, console_com1, console_com2, console_com3, console_com4, console_com5, console_com6,
	   console_com7, console_com8, console_com9, console_com10, console_com11, console_com12, console_com13, console_com14,
	   console_com15, console_vga0 };


enum { hal_entryReserved = 0, hal_entryTemp, hal_entryAllocated, hal_entryInvalid };


/* TODO: add types for 64-bit architectures */
typedef uint32_t addr_t;
typedef uint32_t sysptr_t;
typedef uint32_t syssize_t;

typedef struct {
	uint32_t imgsz;
} __attribute__((packed)) hal_syspage_t;


typedef struct _mapent_t {
	sysptr_t next, prev;
	int32_t type;

	addr_t start;
	addr_t end;
} __attribute__((packed)) mapent_t;


typedef struct _syspage_prog_t {
	sysptr_t next, prev;

	addr_t start;
	addr_t end;

	sysptr_t argv;

	syssize_t imapSz;
	sysptr_t imaps;

	syssize_t dmapSz;
	sysptr_t dmaps;
} __attribute__((packed)) syspage_prog_t;


typedef struct _syspage_map_t {
	sysptr_t next, prev;

	sysptr_t entries;

	addr_t start;
	addr_t end;

	uint32_t attr;
	unsigned char id;

	sysptr_t name;
} __attribute__((packed)) syspage_map_t;


typedef struct {
	hal_syspage_t hs; /* Specific syspage structure defines per architecture */
	syssize_t size;   /* Syspage size                                        */

	addr_t pkernel; /* Physical address of kernel's beginning */

	sysptr_t maps;  /* Maps list    */
	sysptr_t progs; /* Programs list*/

	uint32_t console; /* Console ID defines in hal */
} __attribute__((packed)) syspage_t;

#endif
