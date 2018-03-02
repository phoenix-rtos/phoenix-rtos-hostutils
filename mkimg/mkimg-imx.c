/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * Make image for i.MX 6ULL
 *
 * Copyright 2018 Phoenix Systems
 * Author: Aleksander Kaminski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>


#define SIZE_PAGE 0x1000
#define SYSPAGESZ_MAX 0x400
#define IMGSZ_MAX 68 * 1024
#define ADDR_OCRAM 0x00907000
#define PADDR_BEGIN 0x80000000
#define PADDR_END (PADDR_BEGIN + 128 * 1024 * 1024 - 1)


typedef struct {
	uint32_t start;
	uint32_t end;

	char cmdline[16];
} syspage_program_t;


typedef struct {
	uint32_t pbegin;
	uint32_t pend;

	uint32_t kernel;
	uint32_t kernelsize;

	uint32_t console;
	char arg[256];

	uint32_t progssz;
	syspage_program_t progs[0];
} syspage_t;


static void syspage_dump(syspage_t *s)
{
	int i;

	printf("\nSyspage:\n");
	printf("\tpaddr begin: 0x%04x\n", s->pbegin);
	printf("\tpaddr end: 0x%04x\n", s->pend);
	printf("\tkernel: 0x%04x\n", s->kernel);
	printf("\tkernelsz: 0x%04x\n", s->kernelsize);
	printf("\tconsole: %d\n", s->console);
	printf("\tArgument: %s\n", s->arg);
	printf("\nPrograms (%u):\n", s->progssz);

	for (i = 0; i < s->progssz; ++i) {
		printf("\t%s: s: 0x%04x e: 0x%04x\n", s->progs[i].cmdline, s->progs[i].start, s->progs[i].end);
	}
}


int main(int argc, const char *argv[])
{
	int kfd, ofd, afd, appcnt, i, j;
	char buff[256];
	size_t cnt, offset = 0;
	syspage_t *syspage;

	if (argc < 5) {
		fprintf(stderr, "Usage: %s [kernel binary image] [output file] [console] [arguments] [app1, app2, ...]\n", argv[0]);

		return -1;
	}

	if ((kfd = open(argv[1], O_RDONLY)) < 0) {
		fprintf(stderr, "Could not open kernel binary %s\n", argv[1]);

		return -1;
	}

	if ((ofd = open(argv[2], O_RDWR | O_TRUNC | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP)) < 0) {
		fprintf(stderr, "Could not open output file %s\n", argv[2]);

		close(kfd);

		return -1;
	}

	appcnt = argc - 5;

	if (sizeof(syspage_t) + appcnt * sizeof(syspage_program_t) > SYSPAGESZ_MAX) {
		fprintf(stderr, "Syspage can't hold more than %lu programs\n", (SYSPAGESZ_MAX - sizeof(syspage_t)) / sizeof(syspage_program_t));

		close(kfd);
		close(ofd);

		return -1;
	}

	while (1) {
		cnt = read(kfd, buff, sizeof(buff));
		if (!cnt)
			break;
		offset += cnt;
		while (cnt)
			cnt -= write(ofd, buff, cnt);
	}

	close(kfd);

	printf("Processed kernel image (%lu bytes)\n", offset);

	if (offset < SYSPAGESZ_MAX) {
		fprintf(stderr, "Kernel's too small\n");

		close(ofd);

		return -1;
	}

	if ((syspage = malloc(sizeof(syspage_t) + appcnt * sizeof(syspage_program_t))) == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for syspage\n", sizeof(syspage_t) + appcnt * sizeof(syspage_program_t));

		close(ofd);

		return -1;
	}

	syspage->pbegin = PADDR_BEGIN;
	syspage->pend = PADDR_END;
	syspage->kernel = 0;
	syspage->kernelsize = offset;
	syspage->console = strtoul(argv[3], NULL, 10);
	strncpy(syspage->arg, argv[4], sizeof(syspage->arg));
	syspage->progssz = appcnt;

	for (i = 0; i < appcnt; ++i) {
		lseek(ofd, offset, SEEK_SET);

		syspage->progs[i].start = offset + ADDR_OCRAM;

		if ((afd = open(argv[5 + i], O_RDONLY)) < 0) {
			fprintf(stderr, "Can't open app file %s\n", argv[5 + i]);

			close(ofd);
			free(syspage);

			return -1;
		}

		while (1) {
			cnt = read(kfd, buff, sizeof(buff));
			if (!cnt)
				break;
			offset += cnt;
			while (cnt)
				cnt -= write(ofd, buff, cnt);
		}

		syspage->progs[i].end = offset + ADDR_OCRAM;

		for (j = strlen(argv[5 + i]); j >= 0 && argv[5 + i][j] != '/'; --j);

		strncpy(syspage->progs[i].cmdline, argv[5 + i] + j + 1, sizeof(syspage->progs[i].cmdline));

		printf("Processed app #%d \"%s\" (%u bytes)\n", i, argv[5 + i], syspage->progs[i].end - syspage->progs[i].start);

		close(afd);
	}

	printf("Total image size: %lu bytes (%s)\n", offset, offset < IMGSZ_MAX ? "OK" : "won't fit in OCRAM");

	lseek(ofd, 0x400 + 36, SEEK_SET);
	write(ofd, &offset, sizeof(offset));

	printf("Writing syspage...\n");

	lseek(ofd, 0x20, SEEK_SET);
	cnt = sizeof(syspage_t) + appcnt * sizeof(syspage_program_t);

	i = 0;
	while (cnt) {
		i = write(ofd, (void *)syspage + i, cnt);
		cnt -= i;
	}

	syspage_dump(syspage);

	close(ofd);
	free(syspage);

	printf("Done.\n");

	return 0;
}
