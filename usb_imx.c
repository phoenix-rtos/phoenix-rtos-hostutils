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

typedef struct _mod_t {
	size_t size;
	char *name;
	void *data;
} mod_t;

char *strip_path(char *path)
{
	char *mod_name;
	size_t sz;
	size_t len = strlen(path);
	size_t idx = len - 1;

	while(path[idx] != '/')
		idx--;
	idx++;

	sz = len - idx;

	if (sz > 63)
		sz = 63;

	if (!sz)
		return NULL;

	mod_name = malloc(sz + 1);

	strncpy(mod_name, path + idx, sz);
	mod_name[sz] = '\0';

	return mod_name;
}

mod_t *load_module(char *path)
{
	mod_t *mod;
	int mod_fd;
	struct stat mod_stat;
	size_t offs, br;

	if ((mod_fd = open(path, O_RDONLY)) < 0) {
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
	mod->name = strip_path(path);
	mod->data = malloc(mod->size);

	if (mod->data == NULL) {
		printf("Failed to allocate file data\n");
		return NULL;
	}

	offs = 0;
	while ((br = read(mod_fd, mod->data + offs, 1024)) > 0)
		offs += br;

	if (br < 0) {
		printf("Read error: %s\n", strerror(errno));
		close(mod_fd);
		return NULL;
	}

	if (offs != mod->size) {
		printf("Read rrror: Read %lu bytes instead of %lu.\n", offs, mod->size);
		close(mod_fd);
		return NULL;
	}

	close(mod_fd);
	return mod;
}

int send_module(libusb_device_handle *dev, mod_t *mod)
{
	int sent;

	libusb_control_transfer(dev, 0x00, 0xff, mod->size >> 16, mod->size, (unsigned char *)mod->name, strlen(mod->name) + 1, 5000);

	if (libusb_claim_interface(dev, 0)) {
		printf("Interface claiming error: %s\n", strerror(errno));
		return 1;
	}

	if (libusb_bulk_transfer(dev, 1, mod->data, mod->size, &sent, 5000) != 0) {
		printf("Transfer error: %s\n", mod->name);
		return 1;
	}

	libusb_release_interface(dev, 0);
	return 0;
}

int usb_imx_dispatch(char *modules)
{
	char *tok;
	mod_t *mod;
	libusb_device_handle *dev = NULL;

	if (!strlen(modules)) {
		printf("No modules to load\n");
		return 1;
	}

	printf("Modules to load: %s\n", modules);

	if (libusb_init(NULL)) {
		printf("libusb error\n");
		return 1;
	}

	printf("Waiting for the device...");
	fflush(stdout);
	while ((dev = libusb_open_device_with_vid_pid(NULL, 0x15a2, 0x007d)) == NULL);

	printf("\rDevice found             \n");

	tok = strtok(modules, ",");

	if (tok == NULL)
		tok = modules;

	while (tok != NULL) {

		if ((mod = load_module(tok)) == NULL) {
			libusb_close(dev);
			libusb_exit(NULL);
			return 1;
		}

		if (send_module(dev, mod)) {
			libusb_close(dev);
			libusb_exit(NULL);
			return 1;
		}

		free(mod);
		tok = strtok(NULL, ",");
	}

	libusb_control_transfer(dev, 0xde, 0xc0, 0xdead, 0xdead, NULL, 0, 5000);
	libusb_close(dev);
	libusb_exit(NULL);
	printf("Transfer complete\n");
	return 0;
}
