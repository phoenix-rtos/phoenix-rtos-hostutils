/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * ELF definitions
 *
 * Copyright 2011 Pawel Pisarczyk
 * Copyright 2001 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 */

#ifndef _ELF_H_
#define _ELF_H_

#include <stdio.h>

#include <hostutils-common/types.h>

typedef u16 Elf32_Half;
typedef u32 Elf32_Word;
typedef u32 Elf32_Addr;
typedef u32 Elf32_Off;
typedef int Elf32_Sword;


#define EI_NIDENT     16

#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_REL       9
#define SHT_DYNSYM    11
#define SHT_LOPROC    0x70000000
#define SHT_HIPROC    0x7fffffff
#define SHT_LOUSER    0x80000000
#define SHT_HIUSER    0xffffffff

#define STT_LOPROC    13
#define STT_HIPROC    15

#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_LOPROC     0x70000000
#define PT_HIPROC     0x7fffffff


#pragma pack(push)
#pragma pack(1)

typedef struct {
	unsigned char e_ident[EI_NIDENT];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off  e_phoff;
	Elf32_Off  e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_hsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
} Elf32_Ehdr;


typedef struct {
	Elf32_Word sh_name;
	Elf32_Word sh_type;
	Elf32_Word sh_flags;
	Elf32_Addr sh_addr;
	Elf32_Off  sh_offset;
	Elf32_Word sh_size;
	Elf32_Word sh_link;
	Elf32_Word sh_info;
	Elf32_Word sh_addralign;
	Elf32_Word sh_entsize;
} Elf32_Shdr;


typedef struct {
	Elf32_Word p_type;
	Elf32_Off  p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
} Elf32_Phdr;


typedef struct {
	Elf32_Word    st_name;
	Elf32_Addr    st_value;
	Elf32_Word    st_size;
	unsigned char st_info;
	unsigned char st_other;
	Elf32_Half    st_shndx;
} Elf32_Sym;

#pragma pack(pop)


#endif
