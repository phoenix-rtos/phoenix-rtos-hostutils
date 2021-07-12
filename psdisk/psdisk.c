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


#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/queue.h>


/* TODO: change access to ptable.h */
#include "../../phoenix-rtos-filesystems/ptable/ptable.h"


#define RESET                       "\033[0m"
#define BOLDWHITE                   "\033[1m\033[37m"


#define HELP_ALIGMENT               "\n%s %-35s %-30s"
#define PARTITION_HEADER_ALIGMENT   "%-10s %10s %10s %10s %10s   %-8s\n"
#define PARTITION_DATA_ALIGMENT     "%-10s %10u %10u %10u %10u   %-12s\n"


#define FILE_EXIST(opts)            (opts & (1 << attr_fileExists))
#define MEMORY_DECLARED(opts)       (opts & (1 << attr_memDeclare))
#define PARTITIONS_DECLARED(opts)   (opts & (1 << attr_partsDeclare))
#define PARTITIONS_REMOVE(opts)     (opts & (1 << attr_partsRemove))
#define PTABLE_WITH_OFFSET(opts)    (opts & (1 << attr_ptableOffset))


#define CREATE_IMG(opts)            (!FILE_EXIST(opts) && MEMORY_DECLARED(opts) && PARTITIONS_DECLARED(opts) && !PARTITIONS_REMOVE(opts))
#define UPDATE_IMG(opts)            (FILE_EXIST(opts) && MEMORY_DECLARED(opts) && (PARTITIONS_DECLARED(opts) || PARTITIONS_REMOVE(opts)))
#define READ_IMG(opts)              (FILE_EXIST(opts) && MEMORY_DECLARED(opts) && !PARTITIONS_DECLARED(opts) && !PARTITIONS_REMOVE(opts))



enum { attr_fileExists = 0, attr_memDeclare, attr_partsDeclare, attr_partsRemove, attr_ptableOffset };

enum { part_save = 1, part_remove, part_update };


struct plist_node_t {
	ptable_partition_t pHeader;
	uint8_t status;

	LIST_ENTRY(plist_node_t) ptrs;
};


struct {
	uint32_t pCnt;
	memory_properties_t mem;
	uint8_t opts;            /* individual bits define whether specific option is declared by user */
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
	printf("\n\t%s <image-path> -m <mem-size,sector-size>", app);
	printf("\n\t%s <image-path> -m <mem-size,sector-size> [options]", app);
	printf("\n\nOptions:");
	printf(HELP_ALIGMENT, "   - m ", "<mem-size,sector-size>", "declare memory's parameters");
	printf(HELP_ALIGMENT, "   - p ", "<name,offset,size,type>", "declare partition");
	printf(HELP_ALIGMENT, "   - r ", "<name>", "remove partition");
	printf(HELP_ALIGMENT, "   - o ", " ", "partition table is located in the last sector of memory");
	printf(HELP_ALIGMENT, "   - h ", " ", "show help");
	printf("\n");
	printf("\nPartition types: \n");
	printf("\t- meterfs = 0x75,\n");
	printf("\t- raw = 0x51.\n");
	printf("\n\n");
}


static void psdisk_showPartsTable(void)
{
	struct plist_node_t *node;

	fseek(psdisk_common.file, 0L, SEEK_END);
	printf("\n");
	printf(BOLDWHITE "File system %s: %ld bytes\n"RESET, psdisk_common.fileName, ftell(psdisk_common.file));
	printf("Memory size %u bytes\n", psdisk_common.mem.memSize);
	printf("Units: sector %u bytes \n", psdisk_common.mem.sectorSize);
	printf("\n");
	printf(BOLDWHITE PARTITION_HEADER_ALIGMENT RESET, "Name", "Start", "End", "Sectors", "Size", "Type");
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		printf(PARTITION_DATA_ALIGMENT, node->pHeader.name, node->pHeader.offset, node->pHeader.offset + node->pHeader.size,
			   node->pHeader.size / psdisk_common.mem.sectorSize, node->pHeader.size, psdisk_getTypeName(node->pHeader.type));
	}
	printf("\n");
}


/* Callbacks to ptable library */

static ssize_t psdisk_readImg(unsigned int addr, void *buff, size_t bufflen)
{
	int offs;

	if (PTABLE_WITH_OFFSET(psdisk_common.opts))
		offs = addr;
	else
		offs = addr - (psdisk_common.mem.memSize - psdisk_common.mem.sectorSize);

	if (fseek(psdisk_common.file, offs, SEEK_SET) != 0) {
		fprintf(stderr,"Cannot change file position, err: %s.\n", strerror(errno));
		return -1;
	}

	return fread(buff, sizeof(uint8_t), bufflen, psdisk_common.file);
}


static ssize_t psdisk_writeImg(unsigned int addr, const void *buff, size_t bufflen)
{
	ssize_t res;
	char *wBuff;

	if (bufflen > psdisk_common.mem.sectorSize) {
		fprintf(stderr,"Partition table exceeds sector size. Reduce number of partitions or increase sector size.");
		return -1;
	}

	if (fseek(psdisk_common.file, 0, SEEK_SET) != 0) {
		fprintf(stderr,"Cannot change file position, err: %s.\n", strerror(errno));
		return -1;
	}

	if ((wBuff = (char *)malloc(psdisk_common.mem.sectorSize)) == NULL) {
		fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	memset(wBuff, 0xff, psdisk_common.mem.sectorSize);

	if (PTABLE_WITH_OFFSET(psdisk_common.opts)) {
		while (addr > 0) {
			fwrite(wBuff, sizeof(uint8_t), psdisk_common.mem.sectorSize, psdisk_common.file);
			addr -= psdisk_common.mem.sectorSize;
		}
	}

	memcpy(wBuff, buff, bufflen);
	res = fwrite(wBuff, sizeof(uint8_t), psdisk_common.mem.sectorSize, psdisk_common.file);

	free(wBuff);

	return res;
}


/* Partition table modification functions */

static int psdisk_readPartsTable(void)
{
	int i;
	uint32_t pCnt;
	struct plist_node_t *pNode;
	ptable_partition_t *pHeaders;

	if ((pHeaders = ptable_readPartitions(&pCnt, &psdisk_common.mem)) == NULL) {
		fprintf(stderr,"The file contains incorrect partition table.\n");
		return -1;
	}

	for (i = 0; i < pCnt; ++i) {
		if ((pNode = (struct plist_node_t *)calloc(1, sizeof(struct plist_node_t))) == NULL) {
			fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
			free(pHeaders);
			return -1;
		}
		pNode->pHeader = pHeaders[i];
		LIST_INSERT_HEAD(&psdisk_common.list, pNode, ptrs);
		++psdisk_common.pCnt;
	}

	free(pHeaders);

	return 0;
}


static int psdisk_verifyPartsTable(void)
{
	int i = 0;
	uint32_t pCnt;
	struct plist_node_t *pNode;
	ptable_partition_t *pHeaders;

	/* Verify whether data was saved correctly */
	if ((pHeaders = ptable_readPartitions(&pCnt, &psdisk_common.mem)) == NULL) {
		fprintf(stderr,"The file contains incorrect partition table.\n");
		return -1;
	}

	/* Verify values from file after writing */
	LIST_FOREACH(pNode, &psdisk_common.list, ptrs) {
		if (pNode->status == part_save) {
			if (i >= psdisk_common.pCnt || memcmp(&pNode->pHeader, &pHeaders[i], sizeof(ptable_partition_t)) != 0) {
				free(pHeaders);
				return -1;
			}
			++i;
		}
	}

	free(pHeaders);

	return 0;
}


static int psdisk_createPartsTable(void)
{
	int i = 0;
	struct plist_node_t *node;
	ptable_partition_t *pHeaders;

	if ((pHeaders = (ptable_partition_t *)calloc(psdisk_common.pCnt, sizeof(ptable_partition_t))) == NULL) {
		fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	/* Copy partition table to temporary array */
	LIST_FOREACH(node, &psdisk_common.list, ptrs) {
		if (node->status == part_save)
			memcpy(&pHeaders[i++], &node->pHeader, sizeof(ptable_partition_t));
	}

	if (ptable_writePartitions(pHeaders, psdisk_common.pCnt, &psdisk_common.mem) < 0) {
		fprintf(stderr,"Cannot write partition table to file %s.\n", psdisk_common.fileName);
		free(pHeaders);
		return -1;
	}

	free(pHeaders);

	return 0;
}


static int psdisk_createImg(void)
{
	if (psdisk_createPartsTable() < 0)
		return -1;

	if (psdisk_verifyPartsTable() < 0)
		return -1;

	psdisk_showPartsTable();

	return 0;
}


static int psdisk_updatePartsList(ptable_partition_t *pHeaders, uint32_t pCnt)
{
	int i, action;
	struct plist_node_t *pNode, *newNode, *tempNode;

	for (i = 0; i < pCnt; ++i) {
		/* Default action: partition from file differs from partition defines by user. */
		action = part_save;

		LIST_FOREACH(pNode, &psdisk_common.list, ptrs) {
			if (strncmp((const char *)&pNode->pHeader.name, (const char *)&pHeaders[i].name, sizeof(pNode->pHeader.name)) == 0) {
				/* Remove partition declare by user */
				if (pNode->status == part_remove) {
					action = part_remove;
					break;
				}
				/* Partition from file is updated by new one defines by user. */
				else if (pNode->status == part_save) {
					action = part_update;
					break;
				}
			}
		}

		switch (action) {
			case part_save:
				if ((newNode = (struct plist_node_t *)malloc(sizeof(struct plist_node_t))) == NULL) {
					fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
					return -1;
				}
				memcpy(&newNode->pHeader, &pHeaders[i], sizeof(ptable_partition_t));
				newNode->status = part_save;

				LIST_INSERT_HEAD(&psdisk_common.list, newNode, ptrs);
				++psdisk_common.pCnt;
				break;

			case part_remove:
				tempNode = LIST_NEXT(pNode, ptrs);
				LIST_REMOVE(pNode, ptrs);
				free(pNode);
				pNode = tempNode;
				--psdisk_common.pCnt;
				break;

			case part_update:
				break;

			default:
				break;
		}
	}

	/* Check whether user define correct partition to remove */
	LIST_FOREACH(pNode, &psdisk_common.list, ptrs) {
		if (pNode->status == part_remove) {
			fprintf(stderr,"ERROR: cannot remove %s partition. It is not located in %s.\n", pNode->pHeader.name, psdisk_common.fileName);
			return -1;
		}
	}

	return 0;
}


static int psdisk_updateImg(void)
{
	uint32_t pCnt;
	ptable_partition_t *pHeaders;

	if ((pHeaders = ptable_readPartitions(&pCnt, &psdisk_common.mem)) == NULL) {
		fprintf(stderr,"The file contains incorrect partition table.\n");
		return -1;
	}

	if (psdisk_updatePartsList(pHeaders, pCnt) < 0) {
		free(pHeaders);
		return -1;
	}

	if (psdisk_createImg() < 0) {
		free(pHeaders);
		return -1;
	}

	free(pHeaders);

	return 0;
}


/* Parsing data from user */

static int psdisk_parseToRm(void)
{
	int i;
	struct plist_node_t *pNode;

	/* Parse partition name */
	for (i = 0; i < strlen(optarg); ++i) {
		if (!isalnum(optarg[i])) {
			fprintf(stderr,"Invalid partition name - %s.\n", optarg);
			return -1;
		}
	}

	if ((pNode = (struct plist_node_t *)calloc(1, sizeof(struct plist_node_t))) == NULL) {
		fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	if (strlen(optarg) >= sizeof(pNode->pHeader.name)) {
		fprintf(stderr,"Invalid partition name - %s.\n", optarg);
		free(pNode);
		return -1;
	}
	else {
		strncpy((char *)pNode->pHeader.name, optarg, sizeof(pNode->pHeader.name));
	}

	pNode->status = part_remove;

	LIST_INSERT_HEAD(&psdisk_common.list, pNode, ptrs);
	++psdisk_common.pCnt;

	return 0;
}


static int psdisk_parseToSave(void)
{
	int i, ret = 0;
	char *argsStr, *endptr;
	const char *delimiter = ",";
	struct plist_node_t *pNode = NULL;

	const int MAX_PARTITION_OPTS = 4;
	char *opts[MAX_PARTITION_OPTS];

	/* Get data from optarg and separete them */
	if ((argsStr = (char *)malloc(strlen(optarg) + 1)) == NULL) {
		fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	do {
		strcpy(argsStr, optarg);
		if ((opts[0] = strtok(argsStr, delimiter)) == NULL) {
			fprintf(stderr,"Invalid number of arguments.\n");
			break;
		}

		for (i = 1; i < MAX_PARTITION_OPTS; ++i) {
			if ((opts[i] = strtok(NULL, delimiter)) == NULL) {
				fprintf(stderr,"Invalid number of arguments.\n");
				ret = 1;
				break;
			}
		}

		if (ret)
			break;

		if ((pNode = (struct plist_node_t *)calloc(1, sizeof(struct plist_node_t))) == NULL) {
			fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
			break;
		}

		/* Parse partition name */
		for (i = 0; i < strlen(opts[0]); ++i) {
			if (!isalnum(opts[0][i])) {
				fprintf(stderr,"Invalid partition name - %s.\n", opts[0]);
				ret = 1;
				break;
			}
		}

		if (ret)
			break;

		if (strlen(opts[0]) >= sizeof(pNode->pHeader.name)) {
			fprintf(stderr,"Invalid partition name - %s.\n", opts[0]);
			break;
		}
		else {
			strncpy((char *)pNode->pHeader.name, opts[0], sizeof(pNode->pHeader.name));
		}

		if (((pNode->pHeader.offset = strtoul(opts[1], &endptr, 0)) == 0 && strlen(endptr) != 0) || (pNode->pHeader.offset != 0 && strlen(endptr) != 0)) {
			fprintf(stderr,"Partition %s has invalid offset %s.\n", pNode->pHeader.name, endptr);
			break;
		}

		if ((pNode->pHeader.size = strtoul(opts[2], &endptr, 0)) == 0 || strlen(endptr) != 0) {
			fprintf(stderr,"Partition %s has invalid size %s.\n", pNode->pHeader.name, endptr);
			break;
		}

		if ((pNode->pHeader.type = strtoul(opts[3], &endptr, 0)) == 0 || strlen(endptr) != 0) {
			fprintf(stderr,"Partition %s has invalid type %s.\n", pNode->pHeader.name, endptr);
			break;
		}

		pNode->status = part_save;

		LIST_INSERT_HEAD(&psdisk_common.list, pNode, ptrs);
		++psdisk_common.pCnt;

		free(argsStr);
		return 0;

	} while(0);

	free(pNode);
	free(argsStr);

	return -1;
}


static int psdisk_parseMem(void)
{
	char *argsStr, *endptr;
	const char *delimiter = ",";

	const int MAX_MEM_OPTS = 2;
	char *opts[MAX_MEM_OPTS];

	/* Get data from optarg and separete them */
	if ((argsStr = malloc(strlen(optarg) + 1)) == NULL) {
		fprintf(stderr,"Cannot allocate memory, err: %s.\n", strerror(errno));
		return -1;
	}

	strcpy(argsStr, optarg);
	/* Separate arguments */
	if ((opts[0] = strtok(argsStr, delimiter)) == NULL || (opts[1] = strtok(NULL, delimiter)) == NULL) {
		fprintf(stderr,"Invalid number of arguments.\n");
		free(argsStr);
		return -1;
	}

	if ((psdisk_common.mem.memSize = strtoul(opts[0], &endptr, 0)) == 0 || strlen(endptr) != 0) {
		fprintf(stderr,"Invalid memory size.\n");
		free(argsStr);
		return -1;
	}

	if ((psdisk_common.mem.sectorSize = strtoul(opts[1], &endptr, 0)) == 0 || strlen(endptr) != 0) {
		fprintf(stderr,"Invalid sector siz.\n");
		free(argsStr);
		return -1;
	}

	psdisk_common.mem.read = psdisk_readImg;
	psdisk_common.mem.write = psdisk_writeImg;

	free(argsStr);

	return 0;
}


static int psdisk_openImg(char *argv[])
{
	const char* param;

	if ((psdisk_common.fileName = argv[1]) == NULL) {
		fprintf(stderr,"Invalid file name.\n");
		return -1;
	}

	if (psdisk_common.fileName[0] == '-') {
		fprintf(stderr,"First argument has to be a file name.\n");
		return -1;
	}

	/* Check whether file exists. */
	if (access(psdisk_common.fileName, F_OK) == 0)
		psdisk_common.opts |= 1 << attr_fileExists;

	if (FILE_EXIST(psdisk_common.opts))
		param = "r+b";
	else
		param = "w+b";

	if ((psdisk_common.file = fopen(psdisk_common.fileName, param)) == NULL) {
		fprintf(stderr,"Cannot open file - %s, err: %s.\n", psdisk_common.fileName, strerror(errno));
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

	if (psdisk_common.file != NULL)
		fclose(psdisk_common.file);
}


static int psdisk_handleImg(void)
{
	int res = -1;
	int rmFile = 0;

	if (CREATE_IMG(psdisk_common.opts)) {
		if ((res = psdisk_createImg()) < 0) {
			fprintf(stderr,"Cannot create file system image '%s'.\n", psdisk_common.fileName);
			rmFile = 1;
		}
		else {
			printf("File system image '%s' was created correctly.\n", psdisk_common.fileName);
		}
	}
	else if (UPDATE_IMG(psdisk_common.opts)) {
		if ((res = psdisk_updateImg()) < 0)
			fprintf(stderr,"Cannot update file system image '%s'.\n", psdisk_common.fileName);
		else
			printf("File system image '%s' was updated correctly.\n", psdisk_common.fileName);

	}
	else if (READ_IMG(psdisk_common.opts)) {
		if ((res = psdisk_readPartsTable()) < 0)
			fprintf(stderr,"Cannot read file system image '%s'.\n", psdisk_common.fileName);
		else
			psdisk_showPartsTable();
	}
	else {
		fprintf(stderr,"Inappropriate option, read help.\n");
		rmFile = !FILE_EXIST(psdisk_common.opts);
	}

	psdisk_destroy();
	if (rmFile)
		remove(psdisk_common.fileName);

	return res;
}


int main(int argc, char *argv[])
{
	int opt;

	psdisk_common.pCnt = 0;
	psdisk_common.opts = 0;
	LIST_INIT(&psdisk_common.list);

	if (argc < 2) {
		fprintf(stderr, "%s: bad usage\n", argv[0]);
		fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
		return -1;
	}

	/* First argument has to be an image path or help option. */
	if (strcmp(argv[1], "-h") != 0) {
		if (psdisk_openImg(argv) < 0)
			return -1;
	}

	while ((opt = getopt(argc, argv, "r:m:p:ho")) != -1) {
		switch (opt) {
			case 'm':
				if (!MEMORY_DECLARED(psdisk_common.opts)) {
					if (psdisk_parseMem() < 0) {
						psdisk_destroy();
						return -1;
					}
					psdisk_common.opts |= 1 << attr_memDeclare;
				}
				else {
					fprintf(stderr,"Only one memory declarion is possible.\n");
					return -1;
				}
				break;

			case 'p':
				if (psdisk_parseToSave() < 0) {
					psdisk_destroy();
					return -1;
				}
				psdisk_common.opts |= 1 << attr_partsDeclare;
				break;

			case 'r':
				if (psdisk_parseToRm() < 0) {
					psdisk_destroy();
					return -1;
				}
				psdisk_common.opts |= 1 << attr_partsRemove;
				break;

			case 'o':
				psdisk_common.opts |= 1 << attr_ptableOffset;
				break;

			case 'h':
				psdisk_printHelp(argv[0]);
				psdisk_destroy();
				if (!FILE_EXIST(psdisk_common.opts))
					remove(psdisk_common.fileName);
				return 0;

			default:
				fprintf(stderr,"Unknown option: %c.\n", optopt);
				psdisk_destroy();
				if (!FILE_EXIST(psdisk_common.opts))
					remove(psdisk_common.fileName);
				return -1;
		}
	}

	return psdisk_handleImg();
}
