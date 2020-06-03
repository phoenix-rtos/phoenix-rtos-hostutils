/*
 * Phoenix-RTOS
 *
 * Phoenix Systems disk tool
 *
 * Copyright 2020 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/list.h>

/* TODO: change access to ptable.h */
#include "../../phoenix-rtos-filesystems/ptable/ptable.h"


#define LOG_ERROR(str, ...) do { fprintf(stderr, __FILE__  ":%d error: " str "\n", __LINE__, ##__VA_ARGS__); } while (0)


/* TODO: create partitions list */
struct {
	uint32_t pCnt;
	memory_properties_t mem;

	FILE *file;
} psdisk_common;


static void psdisk_help(void)
{
	printf("\nUsage:");
	printf("\n\tpsdisk [options]");
	printf("\n\nOptions");
	printf("\n%s %-35s %-30s", "   - i ", "<path-to-image-output>", "= Specify image path");
	printf("\n%s %-35s %-30s", "   - s ", "<mem-size;sector-size>", "= Specify memory's parameters");
	printf("\n%s %-35s %-30s", "   - p ", "<name;offset;size;type-hex-value>", "= Specify partition's parameters");
	printf("\n%s %-35s %-30s", "       ", " ", "   types: raw partition = 0x51,");
	printf("\n%s %-35s %-30s", "       ", " ", "          meterfs partition = 0x75.");
	printf("\n%s %-35s %-30s", "   - h ", " ", "= Show help");
	printf("\n\n");
}


static ssize_t psdisk_writeData(unsigned int addr, const void *buff, size_t bufflen)
{
	/* TO DO: add option to move partition table at the end of an image ?? */
	return fwrite(buff, sizeof(uint8_t), bufflen, psdisk_common.file);
}


static int psdisk_createpTable(void)
{
	psdisk_common.mem.write = psdisk_writeData;

	/* TODO: call ptable_writePartitions function */


	return 0;
}


static int psdisk_parsePartitionOpt(int optind, char *argv[])
{
	int i;
	char *name;
	ptable_partition_t pHeader;

	const int MAX_PARTITION_ATTR = 4;

	for (i = 0; i < MAX_PARTITION_ATTR; ++i) {
		if (argv[optind + i] == NULL) {
			LOG_ERROR("Invalid number of arguments.");
			return -1;
		}
	}

	name = argv[optind];
	for (i = 0; i < strlen(name); ++i) {
		if (!isalnum(name[i])) {
			LOG_ERROR("Invalid partition name - %s.", name);
			return -1;
		}
	}

	if (strlen(name) > sizeof(pHeader.name)) {
		LOG_ERROR("Invalid partition name - %s.", name);
		return -1;
	}
	else {
		strncpy((char *)pHeader.name, name, strlen(name));
	}

	if ((pHeader.offset = strtoul(argv[optind + 1], NULL, 10)) == 0) {
		LOG_ERROR("Partition %s has invalid offset. %d", pHeader.name);
		return -1;
	}

	if ((pHeader.size = strtoul(argv[optind + 2], NULL, 10)) == 0) {
		LOG_ERROR("Partition %s has invalid size.", pHeader.name);
		return -1;
	}

	if ((pHeader.type = strtoul(argv[optind + 3], NULL, 16)) == 0) {
		LOG_ERROR("Partition %s has invalid type.", pHeader.name);
		return -1;
	}

	/* TODO: add pHeader to partitions list */

	return 0;
}


static int psdisk_parseMemOpt(int optind, char *argv[])
{
	if (argv[optind] == NULL || argv[optind + 1] == NULL) {
		LOG_ERROR("Invalid number of arguments.");
		return -1;
	}

	if ((psdisk_common.mem.memSize = strtoul(argv[optind], NULL, 10)) == 0) {
		LOG_ERROR("Invalid memory size.");
		return -1;
	}

	if ((psdisk_common.mem.sectorSize = strtoul(argv[optind + 1], NULL, 10)) == 0) {
		LOG_ERROR("Invalid sector size.");
		return -1;
	}

	return 0;
}


static int psdisk_createImg(int optind, char *argv[])
{
	int i;
	char *name;

	if ((name = argv[optind]) == NULL) {
		LOG_ERROR("Invalid number of arguments.");
		return -1;
	}

	for (i = 0; i < strlen(name); ++i) {
		if (!isalnum(name[i]) && name[i] != '/') {
			LOG_ERROR("Invalid storage name - %s.", name);
			return -1;
		}
	}

	/* Open/create file in binary mode */
	if ((psdisk_common.file = fopen(name, "w+b")) == NULL) {
		LOG_ERROR("Cannot create image file, check path - %s.", name);
		return -1;
	}

	return 0;
}


static void psdisk_destroy(void)
{
	if (psdisk_common.file != NULL)
		fclose(psdisk_common.file);
}


int main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "i:s:p:h")) != -1) {
		switch (opt) {
			case 'i':
				if (psdisk_createImg(optind - 1, argv) < 0)
					return 0;
				break;

			case 's':
				if (psdisk_parseMemOpt(optind - 1, argv) < 0)
					return 0;
				break;

			case 'p':
				if (psdisk_parsePartitionOpt(optind - 1, argv) < 0)
					return 0;
				break;

			case 'h':
				psdisk_help();
				return 0;

			default:
				LOG_ERROR("Unknown option: %c\n", opt);
				return 0;
		}
	}

	if (psdisk_createpTable() < 0)
		return 0;

	printf("Partition table has been created.\n");

	/* TODO: open file and verify partition table ?? */

	psdisk_destroy();

	return 0;
}
