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
 * See the LICENSE
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>
#if defined(__CYGWIN__)
# if !defined(LIBUSB_API_VERSION) || (LIBUSB_API_VERSION < 0x01000106)
#  error "libusb API version too low. Reqiured minimum 0x01000106"
# endif
# define change_libusb_backend(ctx_ptr) libusb_set_option(ctx_ptr, LIBUSB_OPTION_USE_USBDK)
#else
# define change_libusb_backend(ctx_ptr)
#endif

#include "dispatch.h"

#define SIZE_PAGE 0x1000
#define SYSPAGESZ_MAX 0x400
#define IMGSZ_MAX 68 * 1024
#define ADDR_OCRAM 0x00907000
#define ADDR_DDR 0x80000000
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

extern int silent;

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
		mod->args[argsz - 1] = 0;
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

int count_sysprogs(char *initrd, char *console, char *append, int *sysprogs_sz)
{
	int cnt = 0, sz = 0, pfd;
	struct stat pstat;
	char *progs, *prog;

	asprintf(&progs, "%s %s %s", console ? console : " ", initrd ? initrd : " ",  append ? append : " ");
	prog = strtok(progs, " ");

	while (prog) {
		if (!strlen(prog))
			goto next;

		cnt++;
		if ((pfd = open(prog, O_RDONLY)) < 0) {
			fprintf(stderr, "Could not open file %s\n", initrd);
			goto next;
		}

		if (fstat(pfd, &pstat) != 0) {
			printf("File stat error: %s\n", strerror(errno));
			close(pfd);
			goto next;
		}
		sz += pstat.st_size;
		close(pfd);
next:
		prog = strtok(NULL, " ");
	}

	free(progs);
	*sysprogs_sz = sz;
	return cnt;
}

int append_sysprogs(void *image, char *initrd, char *console, char *append, syspage_t *syspage, size_t offset, unsigned int addr)
{
	int i = 0, j = 0, pfd, cnt;
	char *progs, *prog;

	asprintf(&progs, "%s %s %s", console ? console : " ", initrd ? initrd : " ",  append ? append : " ");
	prog = strtok(progs, " ");

	while (prog) {
		if (!strlen(prog))
			goto next;
		syspage->progs[i].start = offset + addr;

		if ((pfd = open(prog, O_RDONLY)) < 0) {
			fprintf(stderr, "Can't open initrd file %s\n", initrd);
			goto next;
		}

		while (1) {
			cnt = read(pfd, image + offset, 256);
			if (!cnt)
				break;
			offset += cnt;
		}

		syspage->progs[i].end = offset + addr;

		for (j = strlen(prog); j >= 0 && prog[j] != '/'; --j);

		strncpy(syspage->progs[i].cmdline, prog + j + 1, sizeof(syspage->progs[i].cmdline) - 1);

		printf("Processed \"%s\" (%u bytes)\n", prog, syspage->progs[i].end - syspage->progs[i].start);

		close(pfd);
next:
		prog = strtok(NULL, " ");
		i++;
	}
	free(progs);
	return offset;
}

int boot_image(char *kernel, char *initrd, char *console, char *append, char *output, int plugin)
{
	int kfd, ifd;
	int err = 0;
	void *image;
	ssize_t size;
	struct stat kstat;
	size_t cnt, offset = 0;
	uint32_t jump_addr, load_addr;
	char *arg = NULL;
	int plugin_sz = 0;
	syspage_t *syspage;
	int sysprogs_cnt = 0, sysprogs_sz = 0;
	unsigned int addr = plugin ? ADDR_DDR : ADDR_OCRAM;


	kernel = strtok(kernel, "=");
	arg = strtok(NULL, "=");

	if ((kfd = open(kernel, O_RDONLY)) < 0) {
		fprintf(stderr, "Could not open kernel binary %s\n", kernel);
		return -1;
	}

	if (fstat(kfd, &kstat) != 0) {
		printf("File stat error: %s\n", strerror(errno));
		close(kfd);
		return -1;
	}

	sysprogs_cnt = count_sysprogs(initrd, console, append, &sysprogs_sz);

	size = kstat.st_size + sysprogs_sz;

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
		return -1;
	}

	if ((syspage = malloc(sizeof(syspage_t) + (sysprogs_cnt * sizeof(syspage_program_t)))) == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for syspage\n", sizeof(syspage_t) + sizeof(syspage_program_t));
		free(image);
		return -1;
	}

	syspage->pbegin = PADDR_BEGIN;
	syspage->pend = PADDR_END;
	syspage->kernel = 0;
	syspage->kernelsize = offset;
	syspage->console = 0;
	strncpy(syspage->arg, arg ? arg : "", sizeof(syspage->arg));
	syspage->progssz = sysprogs_cnt;

	offset = append_sysprogs(image, initrd, console, append, syspage, offset, addr);

	if (plugin) {
		plugin_sz = *(int *)(image + 0x424);
		*(int *)(image + 0x424) = (plugin_sz + 0x199) & ~0x1ff;
		memcpy(image + plugin_sz - 0xc, &offset, sizeof(offset));
	} else
		memcpy(image + 0x400 + 36, &offset, sizeof(offset));

	printf("Writing syspage...\n");

	cnt = sizeof(syspage_t) + (sysprogs_cnt * sizeof(syspage_program_t));

	if (cnt > 0x380) {
		printf("Syspage is too big (too many modules?)\n");
		return -1;
	}
	memcpy(image + 0x20, (void *)syspage, cnt);

	syspage_dump(syspage);

	free(syspage);

	printf("\nTotal image size: %lu bytes.\n\n", offset);

	if (output == NULL) {
		if (plugin) {
			silent = 1;
			printf("Waiting for USB connection...");
			fflush(stdout);
			err = usb_vybrid_dispatch(NULL, (char *)&load_addr, (char *)&jump_addr, image, plugin_sz);
			load_addr = 0x80000000;
			jump_addr = 0x80000000 + plugin_sz - 0x30;
			usleep(500000);
			silent = 0;
			printf("\r                              \r");
			fflush(stdout);
			if (err)
				goto out;
		}
		usb_vybrid_dispatch(NULL, (char *)&load_addr, (char *)&jump_addr, image, offset);
	} else {
		ifd = open(output, O_RDWR | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);

		if (ifd < 0) {
			printf("Output file open error\n");
			return -1;
		}

		cnt = 0;
		while (size) {
			cnt = write(ifd, (void *)image + cnt, size);
			size -= cnt;
		}
		close(ifd);
		chmod(output, S_IRUSR | S_IWUSR);
	}

out:
	free(image);
	return err;
}


int usb_imx_dispatch(char *kernel, char *console, char *initrd, char *append, int plugin)
{
	char *mod_tok, *arg_tok;
	char *mod_p, *arg_p;
	char *modules;
	mod_t *mod;
	libusb_device_handle *dev = NULL;
	int len = 3;

	if (boot_image(kernel, initrd, NULL, NULL, NULL, plugin)) {
		printf("Image booting error. Exiting...\n");
		return -1;
	}

	libusb_context * ctx = NULL;
	if (libusb_init(&ctx)) {
		printf("libusb error\n");
		return 1;
	}
	change_libusb_backend(ctx);

	printf("Waiting for the device to boot...");
	fflush(stdout);
	while ((dev = libusb_open_device_with_vid_pid(NULL, 0x15a2, 0x007d)) == NULL);

	printf("\rDevice booted                    \n");

	if (console != NULL)
		len += strlen(console);

	if (append != NULL)
		len += strlen(append);

	modules = malloc(len);
	memset(modules, 0, len);
	if (console != NULL) {
		modules[0] = 'X';
		strcat(modules, console);
	}

	if (append != NULL && strlen(append)) {
		if (console != NULL)
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
	libusb_exit(ctx);
	ctx = NULL;
	free(modules);
	printf("Transfer complete\n");
	return 0;
}
