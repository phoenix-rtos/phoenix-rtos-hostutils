/*
 * Phoenix-RTOS
 *
 * readmeterfs
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/msg.h>
#include <limits.h>
#include <unistd.h>

#include <host-flashsrv.h>
#include <meterfs.h>


typedef struct file_info_t {
	size_t sectors;
	size_t filesz;
	size_t recordsz;
	size_t recordcnt;
} file_info_t;


#define BUF_SIZE   (8 << 10)
#define FLASHSIZE  (4 * 1024 * 1024)
#define SECTORSIZE (4 * 1024)


static void exitOnRebootTrigger(void)
{
	exit(1);
}


static int file_lookup(const char *name)
{
	id_t id;
	int err;

	err = hostflashsrv_lookup(name, &id);
	if (err < 0) {
		return err;
	}

	return (int)id;
}


static int file_open(const char *name)
{
	id_t id;
	int ret;

	ret = file_lookup(name);
	if (ret < 0) {
		return ret;
	}

	id = ret;

	ret = hostflashsrv_open(&id);
	if (ret < 0) {
		return ret;
	}

	return (int)id;
}


static int file_read(id_t fid, off_t offset, void *buff, size_t bufflen)
{
	return hostflashsrv_readFile(&fid, offset, buff, bufflen);
}


static int file_getInfo(id_t fid, size_t *sectors, size_t *filesz, size_t *recordsz, size_t *recordcnt)
{
	meterfs_i_devctl_t iptr;
	meterfs_o_devctl_t optr;
	int err;

	iptr.type = meterfs_info;
	iptr.id = fid;

	err = hostflashsrv_devctl(&iptr, &optr);
	if (err < 0) {
		return err;
	}

	if (sectors != NULL) {
		(*sectors) = optr.info.sectors;
	}

	if (filesz != NULL) {
		(*filesz) = optr.info.filesz;
	}

	if (recordsz != NULL) {
		(*recordsz) = optr.info.recordsz;
	}

	if (recordcnt != NULL) {
		(*recordcnt) = optr.info.recordcnt;
	}

	return 0;
}


int file_init(const char *path)
{
	size_t filesz = FLASHSIZE;
	size_t sectorsz = SECTORSIZE;
	int err;

	err = hostflashsrv_init(&filesz, &sectorsz, path);
	if (err < 0) {
		(void)printf("hostflashsrv: init failed\n");
	}
	return err;
}


static int fs_setKey(const uint8_t *key, size_t keylen)
{
	meterfs_i_devctl_t i = { 0 };
	meterfs_o_devctl_t o = { 0 };

	/* key needs to be 128 bits */
	if (keylen != 16) {
		return -1;
	}

	i.type = meterfs_setKey;
	memcpy(i.setKey.key, key, keylen);

	return hostflashsrv_devctl(&i, &o);
}


static void file_print(const char *name)
{
	char buf[BUF_SIZE];

	int fd = file_open(name);
	if (fd < 0) {
		(void)fprintf(stderr, "file_open failed: %s\n", strerror(-fd));
		exit(1);
	}

	file_info_t info = { 0 };
	if (file_getInfo(fd, &info.sectors, &info.filesz, &info.recordsz, &info.recordcnt) != 0) {
		fprintf(stderr, "file_getInfo failed");
		exit(1);
	}

	fprintf(stderr, "file: %s\n"
					"sectors: %zu\n"
					"filesz: %zu\n"
					"recordsz: %zu\n"
					"recordcnt: %zu\n",
			name, info.sectors, info.filesz, info.recordsz, info.recordcnt);

	for (size_t r = 0; r < info.recordcnt; r++) {
		int ret = file_read(fd, r * info.recordsz, buf, info.recordsz);

		if (ret < 0) {
			fprintf(stderr, "file_read bad: %d\n", ret);
			exit(1);
		}

		printf("r%05zu", r);

		for (size_t i = 0; i < ret; i++) {
			printf(" %02x", (uint8_t)buf[i]);
		}

		printf(" |");
		for (size_t i = 0; i < ret; i++) {
			char c = buf[i];
			printf("%c", isascii(c) ? c : '.');
		}
		printf("|\n");
	}
}


static void printUsage(const char *name)
{
	fprintf(stderr, "Usage: %s -m meterfs_mount_path [opts]\n"
					"opts:\n"
					" -k meterfs 128-bit key\n"
					" -f file_name\n"
					" -r reboot_trigger\n"
					" -u unreliable_write_trigger\n",
			name);
}


int main(int argc, char **argv)
{
	int opt;

	char *mountPath = NULL;
	char *file = NULL;

	int rebootTrigger = 0;
	int unreliableWriteTrigger = 0;

	uint8_t key[16] = { 0 };
	bool encrypted = false;

	while ((opt = getopt(argc, argv, "hm:f:r:u:k:")) != -1) {
		switch (opt) {
			case 'k':
				encrypted = true;
				size_t len = strnlen(optarg, 2 * sizeof(key) + 1);
				if (len != 2 * 2 * sizeof(uint64_t)) {
					fprintf(stderr, "invalid key len: %zu\n", len);
					return EXIT_FAILURE;
				}
				len /= 2;
				for (size_t i = 0; i < len; i++) {
					if (sscanf(&optarg[2 * i], "%2hhx", &key[i]) != 1) {
						fprintf(stderr, "bad hex in key at position %zu\n", 2 * i);
						return EXIT_FAILURE;
					}
				}
				break;
			case 'm':
				mountPath = optarg;
				break;
			case 'f':
				file = optarg;
				break;
			case 'r':
				errno = 0;
				rebootTrigger = strtoul(optarg, NULL, 10);
				if (errno != 0) {
					(void)printf("bad reboot trigger: %s (%d)\n", optarg, errno);
					return EXIT_FAILURE;
				}

				break;
			case 'u':
				errno = 0;
				unreliableWriteTrigger = strtoul(optarg, NULL, 10);
				if (errno != 0) {
					(void)printf("bad unreliable write trigger: %s (%d)\n", optarg, errno);
					return EXIT_FAILURE;
				}
				break;
			case 'h':
			default:
				printUsage(argv[0]);
				return EXIT_FAILURE;
		}
	}

	if (mountPath == NULL) {
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

#if METERFS_DEBUG_UTILS
	meterfs_debugCtx_t debugCtx = {
		.rebootTrigger = rebootTrigger,
		.unreliableWriteTrigger = unreliableWriteTrigger,
		.dryErase = true,
		.onRebootCb = exitOnRebootTrigger,
	};

	fprintf(stderr, "rebootTrigger=%d\n", rebootTrigger);
	fprintf(stderr, "unreliableWriteTrigger=%d\n", unreliableWriteTrigger);
	hostflashsrv_setDebugCtx(&debugCtx);
#else
	if (rebootTrigger != 0 || unreliableWriteTrigger != 0) {
		fprintf(stderr, "rebootTrigger/unreliableWriteTrigger set, but METERFS_DEBUG_UTILS=0\n");
		return EXIT_FAILURE;
	}
#endif

	if (file_init(mountPath) != 0) {
		(void)printf("Failed to initialize test\n");
		return EXIT_FAILURE;
	}

	if (encrypted && fs_setKey(key, sizeof(key)) != 0) {
		(void)printf("Failed to set key\n");
		return EXIT_FAILURE;
	}

	if (file != NULL) {
		file_print(file);
	}

	return 0;
}
