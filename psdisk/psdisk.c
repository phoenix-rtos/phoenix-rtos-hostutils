/*
 * Phoenix-RTOS
 *
 * Phoenix Systems Disk Tool
 *
 * Copyright 2020, 2023 Phoenix Systems
 * Author: Hubert Buczynski, Lukasz Kosinski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/queue.h>

/* le32toh() and  not defined on MacOS */
#ifdef __APPLE__

#include <libkern/OSByteOrder.h>
#define le32toh(x) OSSwapLittleToHostInt32(x)

#endif

/* TODO: change access to ptable.h */
#include "../../phoenix-rtos-corelibs/libptable/ptable.h"


#define RESET                       "\033[0m"
#define BOLDWHITE                   "\033[1m\033[37m"


#define HELP_ALIGMENT               "\n%s %-35s %-30s"
#define PARTITION_HEADER_ALIGMENT   "%-10s %10s %10s %10s %10s   %-8s\n"
#define PARTITION_DATA_ALIGMENT     "%-10s %10u %10u %10u %10u   %-12s\n"


#define FILE_EXIST(opts)            (opts & (1 << attr_fileExists))
#define MEMORY_DECLARED(opts)       (opts & (1 << attr_memDeclare))
#define PARTITIONS_DECLARED(opts)   (opts & (1 << attr_partsDeclare))
#define PARTITIONS_REMOVE(opts)     (opts & (1 << attr_partsRemove))


#define CREATE_IMG(opts)            (!FILE_EXIST(opts) && MEMORY_DECLARED(opts) && PARTITIONS_DECLARED(opts) && !PARTITIONS_REMOVE(opts))
#define UPDATE_IMG(opts)            (FILE_EXIST(opts) && MEMORY_DECLARED(opts) && (PARTITIONS_DECLARED(opts) || PARTITIONS_REMOVE(opts)))
#define READ_IMG(opts)              (FILE_EXIST(opts) && MEMORY_DECLARED(opts) && !PARTITIONS_DECLARED(opts) && !PARTITIONS_REMOVE(opts))


enum { attr_fileExists = 0, attr_memDeclare, attr_partsDeclare, attr_partsRemove };


enum { part_save = 1, part_remove, part_update };


struct plist_node_t {
	ptable_part_t part;
	uint8_t status;
	LIST_ENTRY(plist_node_t) ptrs;
};


struct {
	uint32_t count;
	uint32_t memsz;
	uint32_t blksz;
	uint8_t opts; /* Individual bits define whether specific option is declared by user */
	char *fileName;
	FILE *file;
	LIST_HEAD(plist_t, plist_node_t) list;
} psdisk_common;


/* Information functions */

static const char *psdisk_getTypeName(uint8_t type)
{
	switch (type) {
		case ptable_raw:
			return "raw";

		case ptable_jffs2:
			return "jffs2";

		case ptable_meterfs:
			return "meterfs";

		default:
			return "err";
	}
}


static void psdisk_printHelp(const char *app)
{
	printf("\nUsage:");
	printf("\n\t%s -h", app);
	printf("\n\t%s <image-path> -m <mem-size,block-size>", app);
	printf("\n\t%s <image-path> -m <mem-size,block-size> [options]", app);
	printf("\n\nOptions:");
	printf(HELP_ALIGMENT, "   - m ", "<mem-size,block-size>", "declare memory parameters");
	printf(HELP_ALIGMENT, "   - p ", "<name,offset,size,type>", "declare partition");
	printf(HELP_ALIGMENT, "   - r ", "<name>", "remove partition");
	printf(HELP_ALIGMENT, "   - h ", " ", "show help");
	printf("\n");
	printf("\nPartition types:\n");
	printf("\t- meterfs = 0x75,\n");
	printf("\t- jffs2 = 0x72,\n");
	printf("\t- raw = 0x51.\n");
	printf("\n\n");
}


static void psdisk_showPartsTable(void)
{
	struct plist_node_t *node;

	fseek(psdisk_common.file, 0L, SEEK_END);
	printf("\n");
	printf(BOLDWHITE "Partition table %s: %ld bytes\n"RESET, psdisk_common.fileName, ftell(psdisk_common.file));
	printf("Memory size: %u bytes\n", psdisk_common.memsz);
	printf("Block size: %u bytes\n", psdisk_common.blksz);
	printf("\n");
	printf(BOLDWHITE PARTITION_HEADER_ALIGMENT RESET, "Name", "Start", "End", "Blocks", "Size", "Type");
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		printf(PARTITION_DATA_ALIGMENT, node->part.name, node->part.offset, node->part.offset + node->part.size,
			   node->part.size / psdisk_common.blksz, node->part.size, psdisk_getTypeName(node->part.type));
	}
	printf("\n");
}


/* Partition table read/write functions */

static ptable_t *psdisk_readImg(void)
{
	ptable_t *ptable;
	uint32_t count, size;

	if (fseek(psdisk_common.file, 0, SEEK_SET) != 0) {
		return NULL;
	}

	if (fread(&count, sizeof(count), 1, psdisk_common.file) != 1) {
		return NULL;
	}
	count = le32toh(count);

	size = ptable_size(count);
	if (size > psdisk_common.blksz) {
		return NULL;
	}

	if (fseek(psdisk_common.file, 0, SEEK_SET) != 0) {
		return NULL;
	}

	ptable = malloc(size);
	if (ptable == NULL) {
		return NULL;
	}

	if (fread(ptable, size, 1, psdisk_common.file) != 1) {
		free(ptable);
		return NULL;
	}

	if (ptable_deserialize(ptable, psdisk_common.memsz, psdisk_common.blksz) < 0) {
		free(ptable);
		return NULL;
	}

	return ptable;
}


static int psdisk_writeImg(ptable_t *ptable)
{
	uint32_t size;

	if (fseek(psdisk_common.file, 0, SEEK_SET) != 0) {
		return -1;
	}
	size = ptable_size(ptable->count);

	if (ptable_serialize(ptable, psdisk_common.memsz, psdisk_common.blksz) < 0) {
		return -1;
	}

	if (fwrite(ptable, size, 1, psdisk_common.file) != 1) {
		return -1;
	}

	return 0;
}


/* Partition table modification functions */

static int psdisk_readPartsTable(void)
{
	struct plist_node_t *node;
	ptable_t *ptable;
	uint32_t i;
	int ret = 0;

	ptable = psdisk_readImg();
	if (ptable == NULL) {
		fprintf(stderr, "The file contains incorrect partition table.\n");
		return -1;
	}

	for (i = 0; i < ptable->count; i++) {
		node = calloc(1, sizeof(*node));
		if (node == NULL) {
			fprintf(stderr, "Cannot allocate memory, err: %s.\n", strerror(errno));
			ret = -1;
			break;
		}
		node->part = ptable->parts[i];
		LIST_INSERT_HEAD(&psdisk_common.list, node, ptrs);
		psdisk_common.count++;
	}
	free(ptable);

	return ret;
}


static int psdisk_verifyPartsTable(void)
{
	struct plist_node_t *node;
	ptable_t *ptable;
	int ret = 0, i = 0;

	ptable = psdisk_readImg();
	if (ptable == NULL) {
		fprintf(stderr, "The file contains incorrect partition table.\n");
		return -1;
	}

	/* Verify values from file after writing */
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		if (node->status == part_save) {
			if ((i >= psdisk_common.count) || (strcmp((const char *)&node->part.name, (const char *)&ptable->parts[i].name) != 0) ||
				(node->part.offset != ptable->parts[i].offset) || (node->part.size != ptable->parts[i].size) ||
				(node->part.type != ptable->parts[i].type)) {
				ret = -1;
				break;
			}
			i++;
		}
	}
	free(ptable);

	return ret;
}


static int psdisk_createPartsTable(void)
{
	struct plist_node_t *node;
	ptable_t *ptable;
	uint32_t size;
	int ret = 0, i = 0;

	size = ptable_size(psdisk_common.count);
	if (size > psdisk_common.blksz) {
		fprintf(stderr, "Partition table exceeds block size. Reduce number of partitions or increase block size.\n");
		return -1;
	}

	ptable = calloc(1, size);
	if (ptable == NULL) {
		fprintf(stderr, "Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	/* Prepare partition table */
	ptable->count = psdisk_common.count;
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		if (node->status == part_save) {
			ptable->parts[i++] = node->part;
		}
	}

	if (psdisk_writeImg(ptable) < 0) {
		fprintf(stderr, "Cannot write partition table to file %s.\n", psdisk_common.fileName);
		ret = -1;
	}
	free(ptable);

	return ret;
}


static int psdisk_createImg(void)
{
	if (psdisk_createPartsTable() < 0) {
		return -1;
	}

	if (psdisk_verifyPartsTable() < 0) {
		return -1;
	}

	psdisk_showPartsTable();

	return 0;
}


static int psdisk_updatePartsList(ptable_t *ptable)
{
	struct plist_node_t *node, *newNode, *tempNode;
	int action;
	uint32_t i;

	for (i = 0; i < ptable->count; i++) {
		/* Default action: partition from file differs from partition defined by the user */
		action = part_save;

		LIST_FOREACH(node, &psdisk_common.list, ptrs) {
			if (strcmp((const char *)&node->part.name, (const char *)&ptable->parts[i].name) == 0) {
				/* Remove partition defined by the user */
				if (node->status == part_remove) {
					action = part_remove;
					break;
				}
				/* Update partition from file */
				else if (node->status == part_save) {
					action = part_update;
					break;
				}
			}
		}

		switch (action) {
			case part_save:
				newNode = calloc(1, sizeof(*newNode));
				if (newNode == NULL) {
					fprintf(stderr, "Cannot allocate memory, err: %s.\n", strerror(errno));
					return -1;
				}
				newNode->part = ptable->parts[i];
				newNode->status = part_save;
				LIST_INSERT_HEAD(&psdisk_common.list, newNode, ptrs);
				psdisk_common.count++;
				break;

			case part_remove:
				tempNode = LIST_NEXT(node, ptrs);
				LIST_REMOVE(node, ptrs);
				free(node);
				node = tempNode;
				psdisk_common.count--;
				break;

			case part_update:
				break;

			default:
				break;
		}
	}

	/* Check whether user defined correct partition to remove */
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		if (node->status == part_remove) {
			fprintf(stderr, "ERROR: cannot remove %s partition. It is not located in %s.\n", node->part.name, psdisk_common.fileName);
			return -1;
		}
	}

	return 0;
}


static int psdisk_updateImg(void)
{
	ptable_t *ptable;
	int err;

	ptable = psdisk_readImg();
	if (ptable == NULL) {
		fprintf(stderr, "The file contains incorrect partition table.\n");
		return -1;
	}

	err = psdisk_updatePartsList(ptable);
	free(ptable);
	if (err < 0) {
		return -1;
	}

	if (psdisk_createImg() < 0) {
		return -1;
	}

	return 0;
}


/* Parsing data from user */

static int psdisk_parseToRm(const char *arg)
{
	struct plist_node_t *node;
	size_t len;

	/* Parse partition name */
	len = strnlen(arg, sizeof(node->part.name));
	if ((len == 0) || (len >= sizeof(node->part.name))) {
		fprintf(stderr, "Invalid partition name - %s.\n", arg);
		return -1;
	}

	node = calloc(1, sizeof(*node));
	if (node == NULL) {
		fprintf(stderr, "Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	strcpy((char *)node->part.name, arg);
	node->status = part_remove;
	LIST_INSERT_HEAD(&psdisk_common.list, node, ptrs);
	psdisk_common.count++;

	return 0;
}


static int psdisk_parseToSave(const char *arg)
{
	struct plist_node_t *node;
	const char *nptr;
	char *endptr;
	size_t len;

	node = calloc(1, sizeof(*node));
	if (node == NULL) {
		fprintf(stderr, "Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	/* Parse partition name */
	for (len = 0; len < sizeof(node->part.name); len++) {
		if ((arg[len] == ',') || (arg[len] == '\0')) {
			break;
		}
	}

	if ((len == 0) || (len >= sizeof(node->part.name)) || (arg[len] != ',')) {
		fprintf(stderr, "Invalid partition name - %s.\n", arg);
		free(node);
		return -1;
	}
	strncpy((char *)node->part.name, arg, len);
	node->part.name[len] = '\0';

	/* Parse partition offset */
	nptr = arg + len + 1;
	node->part.offset = strtoul(nptr, &endptr, 0);
	if ((endptr == nptr) || (*endptr != ',')) {
		fprintf(stderr, "Invalid partition offset - %s.\n", arg);
		free(node);
		return -1;
	}

	/* Parse partition size */
	nptr = endptr + 1;
	node->part.size = strtoul(nptr, &endptr, 0);
	if ((endptr == nptr) || (*endptr != ',')) {
		fprintf(stderr, "Invalid partition size - %s.\n", arg);
		free(node);
		return -1;
	}

	/* Parse partition type */
	nptr = endptr + 1;
	node->part.type = strtoul(nptr, &endptr, 0);
	if ((endptr == nptr) || (*endptr != '\0')) {
		fprintf(stderr, "Invalid partition type - %s.\n", arg);
		free(node);
		return -1;
	}

	node->status = part_save;
	LIST_INSERT_HEAD(&psdisk_common.list, node, ptrs);
	psdisk_common.count++;

	return 0;
}


static int psdisk_parseMem(const char *arg)
{
	const char *nptr;
	char *endptr;

	nptr = arg;
	psdisk_common.memsz = strtoul(nptr, &endptr, 0);
	if ((endptr == nptr) || (*endptr != ',')) {
		fprintf(stderr, "Invalid memory size - %s.\n", arg);
		return -1;
	}

	nptr = endptr + 1;
	psdisk_common.blksz = strtoul(nptr, &endptr, 0);
	if ((endptr == nptr) || (*endptr != '\0')) {
		fprintf(stderr, "Invalid block size - %s.\n", arg);
		return -1;
	}

	return 0;
}


static int psdisk_openImg(void)
{
	/* Check whether file exists */
	if (access(psdisk_common.fileName, F_OK) == 0) {
		psdisk_common.opts |= 1 << attr_fileExists;
	}

	psdisk_common.file = fopen(psdisk_common.fileName, FILE_EXIST(psdisk_common.opts) ? "r+b" : "w+b");
	if (psdisk_common.file == NULL) {
		fprintf(stderr, "Cannot open file - %s, err: %s.\n", psdisk_common.fileName, strerror(errno));
		return -1;
	}

	return 0;
}


static void psdisk_destroy(void)
{
	struct plist_node_t *node;

	while (!LIST_EMPTY(&psdisk_common.list)) {
		node = LIST_FIRST(&psdisk_common.list);
		LIST_REMOVE(node, ptrs);
		free(node);
	}

	if (psdisk_common.file != NULL) {
		fclose(psdisk_common.file);
	}
}


static int psdisk_handleImg(void)
{
	int ret = -1;
	int rmFile = 0;

	if (CREATE_IMG(psdisk_common.opts)) {
		ret = psdisk_createImg();
		if (ret < 0) {
			fprintf(stderr, "Cannot create partition table image '%s'.\n", psdisk_common.fileName);
			rmFile = 1;
		}
		else {
			printf("Partition table image '%s' was created successfully.\n", psdisk_common.fileName);
		}
	}
	else if (UPDATE_IMG(psdisk_common.opts)) {
		ret = psdisk_updateImg();
		if (ret < 0) {
			fprintf(stderr, "Cannot update partition table image '%s'.\n", psdisk_common.fileName);
		}
		else {
			printf("File system image '%s' was updated successfully.\n", psdisk_common.fileName);
		}
	}
	else if (READ_IMG(psdisk_common.opts)) {
		ret = psdisk_readPartsTable();
		if (ret < 0) {
			fprintf(stderr, "Cannot read partition table image '%s'.\n", psdisk_common.fileName);
		}
		else {
			psdisk_showPartsTable();
		}
	}
	else {
		fprintf(stderr, "Inappropriate option, read help.\n");
		rmFile = !FILE_EXIST(psdisk_common.opts);
	}

	psdisk_destroy();
	if (rmFile != 0) {
		remove(psdisk_common.fileName);
	}

	return ret;
}


int main(int argc, char *argv[])
{
	int opt;

	psdisk_common.count = 0;
	psdisk_common.opts = 0;
	LIST_INIT(&psdisk_common.list);

	if (argc < 2) {
		fprintf(stderr, "%s: bad usage\n", argv[0]);
		fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
		return -1;
	}

	/* First argument has to be an image path or help option */
	if (strcmp(argv[1], "-h") != 0) {
		if (argv[1][0] == '-') {
			fprintf(stderr, "First argument has to be a file name.\n");
			return -1;
		}

		psdisk_common.fileName = argv[1];
		if (psdisk_openImg() < 0) {
			return -1;
		}
	}

	while ((opt = getopt(argc, argv, "r:m:p:h")) != -1) {
		switch (opt) {
			case 'm':
				if (!MEMORY_DECLARED(psdisk_common.opts)) {
					if (psdisk_parseMem(optarg) < 0) {
						psdisk_destroy();
						return -1;
					}
					psdisk_common.opts |= 1 << attr_memDeclare;
				}
				else {
					fprintf(stderr, "Only one memory declarion is possible.\n");
					return -1;
				}
				break;

			case 'p':
				if (psdisk_parseToSave(optarg) < 0) {
					psdisk_destroy();
					return -1;
				}
				psdisk_common.opts |= 1 << attr_partsDeclare;
				break;

			case 'r':
				if (psdisk_parseToRm(optarg) < 0) {
					psdisk_destroy();
					return -1;
				}
				psdisk_common.opts |= 1 << attr_partsRemove;
				break;

			case 'h':
				psdisk_printHelp(argv[0]);
				psdisk_destroy();
				if (!FILE_EXIST(psdisk_common.opts)) {
					remove(psdisk_common.fileName);
				}
				return 0;

			default:
				fprintf(stderr, "Unknown option: %c.\n", optopt);
				psdisk_destroy();
				if (!FILE_EXIST(psdisk_common.opts)) {
					remove(psdisk_common.fileName);
				}
				return -1;
		}
	}

	return psdisk_handleImg();
}
