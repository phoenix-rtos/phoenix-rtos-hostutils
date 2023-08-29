/*
 * Phoenix-RTOS
 *
 * metaELF - Checksum and metadata ELF embedder
 *
 * Copyright 2022 Phoenix Systems
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libgen.h>

#ifdef __APPLE__
#include <libelf/libelf.h>
#else
#include <elf.h>
#endif

#include "bswap.h"


/* Place signature on unused pad bytes */
#define EI_SIGNATURE_VALUE  (EI_PAD)
#define EI_SIGNATURE_METHOD (EI_NIDENT - 1)

/* Supported signatures */
#define SIGNATURE_CRC32 0

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define ENDIANNESS ELFDATA2LSB
#elif __BYTE_ORDER == __BIG_ENDIAN
#define ENDIANNESS ELFDATA2MSB
#else
#error "Invalid host endianness"
#endif


#define _log_prefix         "metaELF: "
#define log_error(fmt, ...) fprintf(stderr, _log_prefix fmt "\n", ##__VA_ARGS__);
#define log_info(fmt, ...)  ((common.quiet == 0) ? printf(_log_prefix fmt "\n", ##__VA_ARGS__) : (void)0);


static struct {
	const char *name;
	union {
		void *memptr;
		unsigned char *ident;
		Elf32_Ehdr *hdr32;
		Elf64_Ehdr *hdr64;
	} elf;
	int quiet;
	enum {
		mode_checkCRC,
		mode_writeCRC,
	} mode;
} common;


extern uint32_t crc32_calc(const uint8_t *buf, uint32_t len, uint32_t base);


static uint16_t uint16(uint16_t val)
{
	return (common.elf.ident[EI_DATA] == ENDIANNESS) ? val : bswap_16(val);
}


static uint32_t uint32(uint32_t val)
{
	return (common.elf.ident[EI_DATA] == ENDIANNESS) ? val : bswap_32(val);
}


static uint64_t uint64(uint64_t val)
{
	return (common.elf.ident[EI_DATA] == ENDIANNESS) ? val : bswap_64(val);
}


static size_t elf32_size(Elf32_Ehdr *e)
{
	return uint32(e->e_shoff) + uint16(e->e_shentsize) * uint16(e->e_shnum);
}


static size_t elf64_size(Elf64_Ehdr *e)
{
	return uint64(e->e_shoff) + uint16(e->e_shentsize) * uint16(e->e_shnum);
}


/* Returns ELF file size based on its metadata */
static ssize_t elf_size(void)
{
	const int elf_class = common.elf.ident[EI_CLASS];

	if (elf_class == ELFCLASS32) {
		return elf32_size(common.elf.hdr32);
	}
	else if (elf_class == ELFCLASS64) {
		return elf64_size(common.elf.hdr64);
	}

	return -1;
}


/* Calculate ELF file checksum in host's byte order */
static uint32_t elf_calc_crc32(uint8_t *ptr, size_t sz, size_t ofs)
{
	uint8_t zero[4] = { 0 };
	uint32_t crc = (uint32_t)-1;
	crc = crc32_calc(ptr, ofs, crc);
	crc = crc32_calc(zero, 4, crc);
	crc = crc32_calc(ptr + ofs + 4, sz - (ofs + 4), crc);

	return ~crc;
}


static int parse_args(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "qwch")) != -1) {
		switch (opt) {
			case 'q':
				common.quiet = 1;
				break;

			case 'w':
				common.mode = mode_writeCRC;
				break;

			case 'c':
				common.mode = mode_checkCRC;
				break;

			case 'h': /* fall-through */
			default:
				printf(
					"Usage: %s [OPTIONS] <file.elf>\n"
					"Options:\n"
					"  -h   Prints this help\n"
					"  -c   Check ELF CRC32 with embedded checksum (default)\n"
					"  -w   Embed CRC32 into ELF file header\n"
					"  -q   Silent mode\n",
					basename(argv[0]));
				return -1;
		}
	}

	if (argc - optind != 1) {
		log_error("No input file");
		return -1;
	}

	common.name = argv[optind];

	return 0;
}


int main(int argc, char **argv)
{
	struct stat st = { 0 };
	uint32_t crcOut, crcIn = 0;
	int fd, ret = EXIT_FAILURE;

	common.elf.memptr = MAP_FAILED;

	if (parse_args(argc, argv) < 0) {
		return EXIT_FAILURE;
	}

	fd = open(common.name, O_RDWR);
	if (fd < 0) {
		log_error("Unable to open file %s", common.name);
		return EXIT_FAILURE;
	}

	do {
		if (fstat(fd, &st) == -1) {
			break;
		}

		if (st.st_size == 0) {
			log_error("File has a zero size");
			break;
		}

		common.elf.memptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (common.elf.memptr == MAP_FAILED) {
			log_error("Unable to mmap file: %s", common.name);
			break;
		}

		if (memcmp(common.elf.ident, ELFMAG, SELFMAG) != 0) {
			log_error("Not an ELF file");
			break;
		}

		if (elf_size() != st.st_size) {
			log_error("The ELF file size on disk does not match its header info");
			break;
		}

		memcpy(&crcIn, &common.elf.ident[EI_SIGNATURE_VALUE], sizeof(uint32_t));

		/* Convert ELF to host byte order */
		crcIn = uint32(crcIn);

		crcOut = elf_calc_crc32(common.elf.memptr, st.st_size, EI_SIGNATURE_VALUE);

		if (common.mode == mode_checkCRC) {
			if (common.elf.ident[EI_SIGNATURE_METHOD] != SIGNATURE_CRC32) {
				log_info("ELF file contains unsupported signature");
				ret = 4;
			}
			if (crcIn == 0 && crcOut != 0) {
				log_info("ELF file does not contain CRC");
				ret = 3;
			}
			else if (crcIn != crcOut) {
				log_info("Integrity error, checksum %08X is invalid", crcIn);
				ret = 2;
			}
			else {
				log_info("Checksum correct %08X", crcIn);
				ret = EXIT_SUCCESS;
			}
		}
		else if (common.mode == mode_writeCRC) {
			if (crcIn != crcOut) {
				log_info("Embedding CRC32=%08X", crcOut);
			}
			else {
				log_info("Already embedded CRC32=%08X", crcOut);
			}

			/* From host to ELF byte order */
			crcOut = uint32(crcOut);
			memcpy(&common.elf.ident[EI_SIGNATURE_VALUE], &crcOut, sizeof(uint32_t));
			common.elf.ident[EI_SIGNATURE_METHOD] = SIGNATURE_CRC32;
			ret = EXIT_SUCCESS;
		}
		else {
			log_info("Calculated CRC is %08X", crcOut);
			ret = EXIT_SUCCESS;
		}

	} while (0);

	(void)close(fd);
	if (common.elf.memptr != MAP_FAILED) {
		(void)munmap(common.elf.memptr, st.st_size);
	}

	return ret;
}
