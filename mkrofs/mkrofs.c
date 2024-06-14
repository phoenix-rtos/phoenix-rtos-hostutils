/*
 * Phoenix-RTOS
 *
 * mkrofs - make Read-Only File System image
 *
 * Copyright 2024 Phoenix Systems
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <unistd.h>
#include <libgen.h>


/*
 * NOTE: this implementation currently is little endian only
 */


#define LOG_prefix    "rofs: "
#define LOG(fmt, ...) fprintf(stdout, LOG_prefix fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, LOG_prefix fmt "\n", ##__VA_ARGS__)


#define ROFS_ALIGNUP(s, sz) (((s) + (sz)-1) & ~((sz)-1))

#define ROFS_SIGNATURE     ((uint8_t[4]) { 'R', 'O', 'F', 'S' })
#define ROFS_HDR_SIGNATURE 0
#define ROFS_HDR_CHECKSUM  4
#define ROFS_HDR_IMAGESIZE 8
#define ROFS_HDR_INDEXOFFS 16
#define ROFS_HDR_NODECOUNT 24
#define ROFS_HEADER_SIZE   64


struct rofs_node {
	uint64_t timestamp;
	uint32_t parentId;
	uint32_t id;
	uint32_t mode;
	uint32_t reserved0;
	int32_t uid;
	int32_t gid;
	uint32_t offset;
	uint32_t reserved1;
	uint32_t size;
	uint32_t reserved2;
	char name[207];
	uint8_t zero; /* contains '\0' */
} __attribute__((packed));


_Static_assert(sizeof(struct rofs_node) == 256, "'struct rofs_node' needs to be 256 bytes");


static struct {
	uint8_t buf[4096];
	struct rofs_node *nodes;
	size_t nodesAllocated;
	size_t nodesCount;
	size_t depthMax;
	size_t depth;
} common = { 0 };


static inline time_t statTimeRecent(struct stat *st)
{
	time_t tim = st->st_ctime;

	if (tim < st->st_atime) {
		tim = st->st_atime;
	}

	if (tim < st->st_mtime) {
		tim = st->st_mtime;
	}

	return tim;
}


static void calc_crc32mem(const uint8_t *buf, uint32_t len, uint32_t *crc)
{
#define CRC32POLY_LE 0xedb88320
	uint32_t register rcrc = *crc;
	int i;
	while (len--) {
		rcrc = (rcrc ^ (*(buf++) & 0xff));
		for (i = 0; i < 8; i++) {
			rcrc = (rcrc >> 1) ^ ((rcrc & 1) ? CRC32POLY_LE : 0);
		}
	}
	*crc = rcrc;
}


static int calc_crc32file(FILE *img, size_t len, uint32_t *crc)
{
	size_t todo = len;

	while (todo > 0) {
		size_t chunksz = (todo > sizeof(common.buf)) ? sizeof(common.buf) : todo;
		size_t rlen = fread(common.buf, 1, chunksz, img);

		if (chunksz != rlen) {
			ERR("fread: %s", strerror(errno));
			return -1;
		}

		if (rlen == 0) {
			break;
		}

		calc_crc32mem(common.buf, rlen, crc);
		todo -= rlen;
	}
	return 0;
}


static int write_header(FILE *img, uint32_t idxOffs, uint32_t imgSize, uint32_t nodeCnt)
{
	uint8_t hdr[ROFS_HEADER_SIZE];

	uint32_t crc = ~0;
	int ret = -1;

	memset(hdr, 0, sizeof(hdr));
	memcpy(&hdr[ROFS_HDR_SIGNATURE], ROFS_SIGNATURE, sizeof(ROFS_SIGNATURE));
	memcpy(&hdr[ROFS_HDR_IMAGESIZE], &imgSize, sizeof(imgSize));
	memcpy(&hdr[ROFS_HDR_INDEXOFFS], &idxOffs, sizeof(idxOffs));
	memcpy(&hdr[ROFS_HDR_NODECOUNT], &nodeCnt, sizeof(nodeCnt));
	calc_crc32mem(hdr + ROFS_HDR_IMAGESIZE, sizeof(hdr) - ROFS_HDR_IMAGESIZE, &crc);

	do {
		if (fseek(img, sizeof(hdr), SEEK_SET) < 0) {
			break;
		}

		if (calc_crc32file(img, imgSize - sizeof(hdr), &crc) < 0) {
			break;
		}

		if (fseek(img, 0, SEEK_SET) < 0) {
			break;
		}

		crc = ~crc;
		memcpy(&hdr[ROFS_HDR_CHECKSUM], &crc, sizeof(crc));

		if (fwrite(hdr, 1, sizeof(hdr), img) != sizeof(hdr)) {
			break;
		}

		ret = 0;
	} while (0);

	LOG("image size: %u", imgSize);
	LOG("node index: %u", idxOffs);
	LOG("CRC32: %08X", crc);

	return ret;
}


static void basestrncpy(char *dst, const char *src, size_t len)
{
	char *tmp;
	if (strlen(src) >= sizeof(common.buf)) {
		ERR("fatal error: path too deep to store in static buffer");
		exit(EXIT_FAILURE);
	}
	tmp = basename(strcpy((char *)common.buf, src));
	if (strlen(tmp) >= len) {
		ERR("Name '%s' will be trimmed to %zu characters", tmp, len);
	}
	strncpy(dst, tmp, len);
}

struct rofs_node *node_alloc(void)
{
	if (common.nodesCount + 1 >= common.nodesAllocated) {
		struct rofs_node *nodes = realloc(common.nodes, (common.nodesAllocated + 128) * sizeof(nodes[0]));
		if (nodes == NULL) {
			ERR("node_alloc: nodes=%zu/%zu: %s", common.nodesCount, common.nodesAllocated, strerror(errno));
			return NULL;
		}
		common.nodesAllocated += 128;
		common.nodes = nodes;
	}

	return memset(&common.nodes[common.nodesCount++], 0, sizeof(struct rofs_node));
}


static int processDir(FILE *img, const char *path, uint32_t parentId, uint32_t *nextId, uint32_t *currOffset)
{
	char *fullpath = NULL;

	struct rofs_node *node;
	struct dirent *entry;
	struct stat st;
	int ret = 0;

	if (common.depth >= common.depthMax) {
		ERR("processDir: max path depth (%zu) reached %s", common.depthMax, path);
		return -ELOOP;
	}

	uint32_t dirId = (*nextId)++;
	DIR *dir = opendir(path);
	if (dir == NULL) {
		ERR("opendir: %s: %s", path, strerror(errno));
		return -errno;
	}

	if (stat(path, &st) < 0) {
		ERR("stat: %s: %s", path, strerror(errno));
		closedir(dir);
		return -errno;
	}

	node = node_alloc();
	if (node == NULL) {
		closedir(dir);
		return -ENOMEM;
	}

	basestrncpy(node->name, path, sizeof(node->name));
	node->zero = '\0'; /* same: node->name[sizeof(node->name) + 1] = '\0'; */
	node->id = dirId;
	node->uid = st.st_uid;
	node->gid = st.st_gid;
	node->mode = st.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH);
	node->timestamp = statTimeRecent(&st);
	node->parentId = parentId;

	common.depth++;

	for (;;) {
		entry = readdir(dir);
		if (entry == NULL) {
			break;
		}

		if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
			continue;
		}

		/****/

		size_t fullpathLen = strlen(path) + 1 + strlen(entry->d_name) + 1;
		char *ptr = realloc(fullpath, fullpathLen);
		if (ptr == NULL) {
			ERR("realloc: path='%s/%s': %s", path, entry->d_name, strerror(errno));
			ret = -errno;
			break;
		}
		fullpath = ptr;

		/****/

		snprintf(fullpath, fullpathLen, "%s/%s", path, entry->d_name);
		if (stat(fullpath, &st) < 0) {
			ERR("stat: %s: %s", fullpath, strerror(errno));
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			ret = processDir(img, fullpath, dirId, nextId, currOffset);
		}
		else if (S_ISREG(st.st_mode)) {
			FILE *file = fopen(fullpath, "r");
			if (file == NULL) {
				ERR("fopen: %s: %s", fullpath, strerror(errno));
				continue;
			}

			LOG("add: %s", fullpath);
			uint32_t file_id = (*nextId)++;
			node = node_alloc();
			if (node == NULL) {
				ret = -ENOMEM;
				break;
			}

			strncpy(node->name, entry->d_name, sizeof(node->name));
			node->zero = '\0'; /* same: node->name[sizeof(node->name) + 1] = '\0'; */
			node->id = file_id;
			node->uid = st.st_uid;
			node->gid = st.st_gid;
			node->mode = st.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH);
			node->offset = *currOffset;
			node->size = (uint32_t)st.st_size;
			node->timestamp = statTimeRecent(&st);
			node->parentId = dirId;

			for (;;) {
				size_t bytes = fread(common.buf, 1, sizeof(common.buf), file);
				if (bytes == 0) {
					break;
				}
				if (fwrite(common.buf, 1, bytes, img) != bytes) {
					ret = -1;
					break;
				}
				*currOffset += bytes;
			}
			fclose(file);
		}
		else {
			LOG("Skipped '%s' as it is not regular file or not directory", fullpath);
		}

		if (ret < 0) {
			break;
		}
	}

	common.depth--;

	free(fullpath);

	closedir(dir);
	return ret;
}


int main(int argc, char *argv[])
{
	FILE *img;
	const char *rootDir;
	const char *imgName;

	uint32_t indexOffset;
	uint32_t fileSize;

	uint32_t nextId = 0;
	uint32_t currOffset = ROFS_HEADER_SIZE;

	if ((argc < 3) || (argc > 4)) {
		printf(
			"Usage: %s <dst> <src> [dep]\n"
			"\tCreate Read-Only File System image\n"
			"Where:\n"
			"\tdst - Destination file system image file name\n"
			"\tsrc - Source root directory to be placed into dst\n"
			"\tdep - Optional recursion MAX_DEPTH, default=128\n",
			argv[0]);

		return EXIT_FAILURE;
	}

	imgName = argv[1];
	rootDir = argv[2];

	common.nodes = NULL;
	common.nodesAllocated = 0;
	common.nodesCount = 0;
	common.depthMax = 128;
	common.depth = 0;

	if (argc == 4) {
		char *end;
		common.depthMax = strtoul(argv[3], &end, 10);
		if ((common.depthMax == 0) || (end[0] != '\0')) {
			ERR("Invalid depth value = '%s'", argv[3]);
			return EXIT_FAILURE;
		}
	}

	LOG("recursion depth: %zu", common.depthMax);

	do {
		img = fopen(imgName, "w+");
		if (img == NULL) {
			break;
		}

		/* Reserve space for the header */
		if (fseek(img, ROFS_HEADER_SIZE, SEEK_SET) < 0) {
			break;
		}

		/* Write content of files into image and build nodes tree */
		int ret = processDir(img, rootDir, -1, &nextId, &currOffset);
		if (ret < 0) {
			errno = -ret;
			break;
		}

		indexOffset = ROFS_ALIGNUP(currOffset, sizeof(struct rofs_node));
		fileSize = indexOffset + sizeof(struct rofs_node) * common.nodesCount;

		/* Write padding due to alignment */
		memset(common.buf, 0, sizeof(common.buf));
		if (fwrite(common.buf, 1, indexOffset - currOffset, img) != indexOffset - currOffset) {
			break;
		}

		/* Write nodes tree */
		if (fwrite(common.nodes, sizeof(struct rofs_node), common.nodesCount, img) != common.nodesCount) {
			break;
		}

		/* Rewind to begin of file and write the header */
		if (write_header(img, indexOffset, fileSize, common.nodesCount) < 0) {
			break;
		}

		fclose(img);
		free(common.nodes);

		LOG("image '%s' created successfully", imgName);

		return EXIT_SUCCESS;
	} while (0);

	ERR("error: %s: %s", imgName, strerror(errno));

	if (img != NULL) {
		fclose(img);
		/* Maybe? unlink(imgName); */
	}

	free(common.nodes);

	return EXIT_FAILURE;
}
