/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * load modules for i.MX 6ULL
 *
 * Copyright 2018 Phoenix Systems
 * Author: Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "dispatch.h"

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


typedef struct _mod_t {
	size_t size;
	char *name;
	char *args;
	void *data;
} mod_t;


char *base_name(char *path)
{
	char *mod_name;
	size_t sz;
	size_t len = strlen(path);
	int i;

	if (len == 0)
		return NULL;

	for (i = len; i >= 0 && path[i] != '/'; i--);

	i++;
	if (i == 0 && (path[0] == 'X' || path[0] == 'F'))
		i++;
	sz = len - i;

	/* max name lenght */
	if (sz > 62)
		sz = 62;

	if (!sz)
		return NULL;

	mod_name = malloc(sz + 2);
	if (path[0] == 'X')
		mod_name[0] = path[0];
	else
		mod_name[0] = 'F';

	strncpy(mod_name + 1, path + i, sz);
	mod_name[sz + 1] = '\0';
	return mod_name;
}


mod_t *load_module(char *path)
{
	mod_t *mod;
	int mod_fd;
	struct stat mod_stat;
	size_t offs, br;
	int i = 0;

	if (path[0] == 'X' || path[0] == 'F')
		i++;

	if ((mod_fd = open(path + i, O_RDONLY)) < 0) {
		printf("Cannot open file %s: %s\n", path, strerror(errno));
		return NULL;
	}

	if (fstat(mod_fd, &mod_stat) != 0) {
		printf("File stat error: %s\n", strerror(errno));
		close(mod_fd);
		return NULL;
	}

	mod = malloc(sizeof(mod_t));

	mod->size = mod_stat.st_size;
	mod->name = base_name(path);
	mod->data = malloc(mod->size);

	if (mod->data == NULL) {
		printf("Failed to allocate file data\n");
		return NULL;
	}

	offs = 0;
	br = 1;
	while (br > 0 && offs < mod->size) {
		br = read(mod_fd, mod->data + offs, mod->size - offs > 1024 ? 1024 : mod->size - offs);
		offs += br;
	}

	if (br < 0) {
		printf("Read error: %s\n", strerror(errno));
		close(mod_fd);
		return NULL;
	}

	if (offs != mod->size) {
		printf("Read error: Read %lu bytes instead of %lu.\n", offs, mod->size);
		close(mod_fd);
		return NULL;
	}

	close(mod_fd);
	return mod;
}


int send_module(libusb_device_handle *dev, mod_t *mod)
{
	int sent = 0;
	int argsz = 0;
	int err = 0;

	libusb_control_transfer(dev, 0x00, 0xff, mod->size >> 16, mod->size, (unsigned char *)mod->name, strlen(mod->name) + 1, 5000);

	if (libusb_claim_interface(dev, 0)) {
		printf("Interface claiming error: %s\n", strerror(errno));
		return 1;
	}

	if (mod->args != NULL)
		argsz = strlen(mod->args) + 1;

	if (argsz > 128) {
		printf("Argument list is too long\n");
		argsz = 128;
	}
	if ((err = libusb_bulk_transfer(dev, 1, (unsigned char *)mod->args, argsz, &sent, 5000)) != 0) {
		printf("Arguments transfer error: %s %s\n", mod->name, libusb_error_name(err));
		return 1;
	}

	if ((err = libusb_bulk_transfer(dev, 1, mod->data, mod->size, &sent, 5000)) != 0) {
		printf("Transfer error: %s should be: %lu sent: %d (%s)\n", mod->name, mod->size, sent, libusb_error_name(err));
		return 1;
	}

	libusb_release_interface(dev, 0);
	return 0;
}


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


int boot_image(char *kernel, char *initrd, char *output)
{
	int kfd, ifd, i, j;
	void *image;
	ssize_t size;
	struct stat kstat, istat;
	size_t cnt, offset = 0;
	uint32_t jump_addr, load_addr;
	syspage_t *syspage;

	if ((kfd = open(kernel, O_RDONLY)) < 0) {
		fprintf(stderr, "Could not open kernel binary %s\n", kernel);
		return -1;
	}

	if (fstat(kfd, &kstat) != 0) {
		printf("File stat error: %s\n", strerror(errno));
		close(kfd);
		return -1;
	}

	if (initrd != NULL) {
		if ((ifd = open(initrd, O_RDONLY)) < 0) {
			fprintf(stderr, "Could not open file %s\n", initrd);
			close(kfd);
			return -1;
		}

		if (fstat(ifd, &istat) != 0) {
			printf("File stat error: %s\n", strerror(errno));
			close(kfd);
			close(ifd);
			return -1;
		}
		close(ifd);
	} else istat.st_size = 0;

	size = kstat.st_size + istat.st_size;

	image = malloc(size);

	if (image == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for image\n", size);
		return -1;
	}

	while (1) {
		cnt = read(kfd, image + offset, 256);
		if (!cnt)
			break;
		if (offset == 0x400) {
			jump_addr = *(uint32_t *)(image + offset + 20); //ivt self ptr
			load_addr = *(uint32_t *)(image + offset + 32); //ivt load address
		}
		offset += cnt;
	}

	close(kfd);

	printf("Processed kernel image (%lu bytes)\n", offset);

	if (offset < SYSPAGESZ_MAX) {
		fprintf(stderr, "Kernel's too small\n");
		free(image);
		close(ifd);
		return -1;
	}

	if ((syspage = malloc(sizeof(syspage_t) + sizeof(syspage_program_t))) == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for syspage\n", sizeof(syspage_t) + sizeof(syspage_program_t));
		free(image);
		close(ifd);
		return -1;
	}

	syspage->pbegin = PADDR_BEGIN;
	syspage->pend = PADDR_END;
	syspage->kernel = 0;
	syspage->kernelsize = offset;
	syspage->console = 0;
	strncpy(syspage->arg, "", sizeof(syspage->arg));
	syspage->progssz = initrd ? 1 : 0;

	if (initrd != NULL) {
		syspage->progs[0].start = offset + ADDR_OCRAM;

		if ((ifd = open(initrd, O_RDONLY)) < 0) {
			fprintf(stderr, "Can't open initrd file %s\n", initrd);
			free(syspage);
			return -1;
		}

		while (1) {
			cnt = read(ifd, image + offset, 256);
			if (!cnt)
				break;
			offset += cnt;
		}

		syspage->progs[0].end = offset + ADDR_OCRAM;

		for (j = strlen(initrd); j >= 0 && initrd[j] != '/'; --j);

		strncpy(syspage->progs[0].cmdline, initrd + j + 1, sizeof(syspage->progs[i].cmdline));

		printf("Processed initrd \"%s\" (%u bytes)\n", initrd, syspage->progs[0].end - syspage->progs[0].start);

		close(ifd);
	}

	memcpy(image + 0x400 + 36, &offset, sizeof(offset));
	printf("Writing syspage...\n");

	cnt = sizeof(syspage_t) + sizeof(syspage_program_t);

	memcpy(image + 0x20, (void *)syspage, cnt);

	syspage_dump(syspage);

	free(syspage);

	printf("\nTotal image size: %lu bytes (%s)\n\n", offset, offset < IMGSZ_MAX ? "\033[32;1mOK\033[0m" : "\033[31;1mwon't fit in OCRAM\033[0m");

	if (offset >= IMGSZ_MAX) {
		free(image);
		return -1;
	}
	if (output == NULL)
		usb_vybrid_dispatch(NULL, (char *)&load_addr, (char *)&jump_addr, image, size);
	else {
		ifd = open(output, O_RDWR | O_TRUNC | O_CREAT);

		if (ifd < 0) {
			printf("Output file open error\n");
			return -1;
		}

		i = 0;
		while (size) {
			i = write(ifd, (void *)image + i, size);
			size -= i;
		}
		close(ifd);
		chmod(output, S_IRUSR | S_IWUSR);
	}

	free(image);
	return 0;
}


int usb_imx_dispatch(char *kernel ,char *console, char *initrd, char *append)
{
	char *mod_tok, *arg_tok;
	char *mod_p, *arg_p;
	char *modules;
	mod_t *mod;
	libusb_device_handle *dev = NULL;

	if (boot_image(kernel, initrd, NULL)) {
		printf("Image booting error. Exiting...\n");
		return -1;
	}

	if (initrd == NULL)
		return 0;

	if (console == NULL || !strlen(console)) {
		printf("No console specified\n");
		return 1;
	}


	if (libusb_init(NULL)) {
		printf("libusb error\n");
		return 1;
	}

	printf("Waiting for the device to boot...");
	fflush(stdout);
	while ((dev = libusb_open_device_with_vid_pid(NULL, 0x15a2, 0x007d)) == NULL);

	printf("\rDevice booted                    \n");

	modules = malloc(strlen(console) + (append != NULL ? strlen(append) : 0) + 3);
	memset(modules, 0, strlen(console) + (append != NULL ? strlen(append) : 0) + 3);
	modules[0] = 'X';
	strcat(modules, console);
	if (append != NULL && strlen(append)) {
		modules[strlen(console) + 1] = ' ';
		strcat(modules, append);
	}

	mod_tok = strtok_r(modules, " ", &mod_p);

	if (mod_tok == NULL)
		mod_tok = modules;

	while (mod_tok != NULL) {

		arg_tok = strtok_r(mod_tok, "=", &arg_p);
		if ((mod = load_module(arg_tok)) == NULL) {
			libusb_control_transfer(dev, 0xde, 0xc0, 0xdead, 0xdead, NULL, 0, 5000);
			libusb_close(dev);
			libusb_exit(NULL);
			free(modules);
			return 1;
		}

		mod->args = strtok_r(NULL, " ", &arg_p);

		if (send_module(dev, mod)) {
			libusb_control_transfer(dev, 0xde, 0xc0, 0xdead, 0xdead, NULL, 0, 5000);
			libusb_close(dev);
			libusb_exit(NULL);
			free(modules);
			free(mod->data);
			free(mod->name);
			free(mod);
			return 1;
		}
		free(mod->data);
		free(mod->name);
		free(mod);
		mod_tok = strtok_r(NULL, " ", &mod_p);
	}

	libusb_control_transfer(dev, 0xde, 0xc0, 0xdead, 0xdead, NULL, 0, 5000);
	libusb_close(dev);
	libusb_exit(NULL);
	free(modules);
	printf("Transfer complete\n");
	return 0;
}
