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
#include <endian.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <unistd.h>
#include <libgen.h>


#define LOG_prefix    "rofs: "
#define LOG(fmt, ...) fprintf(stdout, LOG_prefix fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, LOG_prefix fmt "\n", ##__VA_ARGS__)


#define ROFS_ALIGNUP(s, sz) (((s) + (sz) - 1) & ~((sz) - 1))

#define ROFS_SIGNATURE     ((uint8_t[4]) { 'R', 'O', 'F', 'S' })
#define ROFS_HDR_SIGNATURE 0
#define ROFS_HDR_CHECKSUM  4
#define ROFS_HDR_IMAGESIZE 8
#define ROFS_HDR_INDEXOFFS 16
#define ROFS_HDR_NODECOUNT 24
#define ROFS_HEADER_SIZE   64

enum endianness {
	endian_little,
	endian_big
};

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
	enum endianness endianness;
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
#define CRC32POLY_BE 0x04c11db7
	uint32_t register rcrc = *crc;
	int i;
	uint32_t poly = (common.endianness == endian_little) ? CRC32POLY_LE : CRC32POLY_BE;

	while (len--) {
		rcrc = (rcrc ^ (*(buf++) & 0xff));
		for (i = 0; i < 8; i++) {
			rcrc = (rcrc >> 1) ^ ((rcrc & 1) ? poly : 0);
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


static void write_u32(uint8_t *buf, uint32_t value)
{
	uint32_t converted;

	if (common.endianness == endian_little) {
		converted = htole32(value);
	}
	else {
		converted = htobe32(value);
	}

	memcpy(buf, &converted, sizeof(converted));
}


static void write_u64(uint8_t *buf, uint64_t value)
{
	uint64_t converted;

	if (common.endianness == endian_little) {
		converted = htole64(value);
	}
	else {
		converted = htobe64(value);
	}

	memcpy(buf, &converted, sizeof(converted));
}


static int write_header(FILE *img, uint32_t idxOffs, uint32_t imgSize, uint32_t nodeCnt)
{
	uint8_t hdr[ROFS_HEADER_SIZE];

	uint32_t crc = ~0;
	int ret = -1;

	memset(hdr, 0, sizeof(hdr));
	memcpy(&hdr[ROFS_HDR_SIGNATURE], ROFS_SIGNATURE, sizeof(ROFS_SIGNATURE));
	write_u32(&hdr[ROFS_HDR_IMAGESIZE], imgSize);
	write_u32(&hdr[ROFS_HDR_INDEXOFFS], idxOffs);
	write_u32(&hdr[ROFS_HDR_NODECOUNT], nodeCnt);
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
		write_u32(&hdr[ROFS_HDR_CHECKSUM], crc);

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
	/*
	 * NOTE: According to basename documentation and implementation, should never happen. Required due to GCC 15.2
	 * See: https://github.com/phoenix-rtos/phoenix-rtos-project/issues/1477
	 */
	if (tmp == NULL) {
		ERR("unexpected error");
		exit(EXIT_FAILURE);
	}
	size_t tmplen = strlen(tmp);
	if (tmplen >= len) {
		ERR("Name '%s' will be trimmed to %zu characters", tmp, len);
		memcpy(dst, tmp, len);
	}
	else {
		memcpy(dst, tmp, tmplen);
		memset(dst + tmplen, 0, len - tmplen);
	}
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

			size_t entrylen = strlen(entry->d_name);
			if (entrylen >= sizeof(node->name)) {
				memcpy(node->name, entry->d_name, sizeof(node->name));
			}
			else {
				memcpy(node->name, entry->d_name, entrylen);
				memset(node->name + entrylen, 0, sizeof(node->name) - entrylen);
			}
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


static void serialize_node(const struct rofs_node *node, uint8_t *buf)
{
	write_u64(buf, node->timestamp);
	write_u32(buf + offsetof(struct rofs_node, parentId), node->parentId);
	write_u32(buf + offsetof(struct rofs_node, id), node->id);
	write_u32(buf + offsetof(struct rofs_node, mode), node->mode);
	write_u32(buf + offsetof(struct rofs_node, reserved0), node->reserved0);
	write_u32(buf + offsetof(struct rofs_node, uid), (uint32_t)node->uid);
	write_u32(buf + offsetof(struct rofs_node, gid), (uint32_t)node->gid);
	write_u32(buf + offsetof(struct rofs_node, offset), node->offset);
	write_u32(buf + offsetof(struct rofs_node, reserved1), node->reserved1);
	write_u32(buf + offsetof(struct rofs_node, size), node->size);
	write_u32(buf + offsetof(struct rofs_node, reserved2), node->reserved2);
	memcpy(buf + offsetof(struct rofs_node, name), node->name, sizeof(node->name));
	*(buf + offsetof(struct rofs_node, zero)) = node->zero;
}


static int write_nodes_tree(FILE *img)
{
	uint8_t buf[256];
	for (size_t i = 0; i < common.nodesCount; ++i) {
		serialize_node(&common.nodes[i], buf);
		if (fwrite(buf, 1, sizeof(buf), img) != sizeof(buf)) {
			return -1;
		}
	}
	return 0;
}


static void usage(const char *name)
{
	printf(
		"Usage: %s [-p depth] [-l/-b] -d <dst> -s <src>\n"
		"\tCreate Read-Only File System image\n"
		"Arguments:\n"
		"\t-p <depth> - Optional recursion MAX_DEPTH, default=128\n"
		"\t-l         - Little endian FS, default\n"
		"\t-b         - Big endian FS\n"
		"\t-d <dst>   - Destination file system image file name (required)\n"
		"\t-s <src>   - Source root directory to be placed into dst (required)\n",
		name);
}


int main(int argc, char *argv[])
{
	FILE *img;
	const char *rootDir = NULL;
	const char *imgName = NULL;

	uint32_t indexOffset;
	uint32_t fileSize;

	uint32_t nextId = 0;
	uint32_t currOffset = ROFS_HEADER_SIZE;

	common.nodes = NULL;
	common.nodesAllocated = 0;
	common.nodesCount = 0;
	common.depthMax = 128;
	common.depth = 0;
	common.endianness = endian_little;

	int opt;
	bool endianSet = false;
	do {
		opt = getopt(argc, argv, "p:d:s:lb");
		switch (opt) {
			case 'p': {
				char *end;
				errno = 0;
				common.depthMax = strtoul(optarg, &end, 10);
				if ((common.depthMax == 0) || (errno != 0) || (end[0] != '\0')) {
					ERR("Invalid depth value = '%s'", optarg);
					return EXIT_FAILURE;
				}
				break;
			}

			case 'l':
				if (endianSet) {
					ERR("Endianness already set");
					return EXIT_FAILURE;
				}
				endianSet = true;
				common.endianness = endian_little;
				break;

			case 'b':
				if (endianSet) {
					ERR("Endianness already set");
					return EXIT_FAILURE;
				}
				endianSet = true;
				common.endianness = endian_big;
				break;

			case 'd':
				imgName = optarg;
				break;

			case 's':
				rootDir = optarg;
				break;

			case -1:
				break;

			default:
				usage(argv[0]);
				return EXIT_FAILURE;
		}
	} while (opt != -1);

	if ((imgName == NULL) || (rootDir == NULL)) {
		ERR("Missing required arguments");
		usage(argv[0]);
		return EXIT_FAILURE;
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
		if (write_nodes_tree(img) < 0) {
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
