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
#include <stdint.h>
#include <arpa/inet.h>

#include <hidapi/hidapi.h>


#include "../phoenixd/dispatch.h"

#define SIZE_PAGE 0x1000
#define SYSPAGESZ_MAX 0x400
#define IMGSZ_MAX 68 * 1024
#define ADDR_OCRAM 0x00907000
#define ADDR_DDR 0x80000000
#define PADDR_BEGIN 0x80000000
#define PADDR_END (PADDR_BEGIN + 128 * 1024 * 1024 - 1)

/* SDP protocol section */
#define SET_CMD_TYPE(b,v) (b)[0]=(b)[1]=(v)
#define SET_ADDR(b,v) *((uint32_t*)((b)+2))=htonl(v)
#define SET_COUNT(b,v) *((uint32_t*)((b)+7))=htonl(v);
#define SET_DATA(b,v) *((uint32_t*)((b)+11))=htonl(v);
#define SET_FORMAT(b,v) (b)[6]=(v);

#define CMD_SIZE 17
#define BUF_SIZE 1025
#define INTERRUPT_SIZE 65

static inline void set_write_file_cmd(unsigned char *b, uint32_t addr, uint32_t size)
{
	SET_CMD_TYPE(b, 0x04);
	SET_ADDR(b, addr);
	SET_COUNT(b, size);
	SET_FORMAT(b, 0x20);
}


static inline void set_jmp_cmd(unsigned char *b, uint32_t addr)
{
	SET_CMD_TYPE(b, 0x0b);
	SET_ADDR(b, addr);
	SET_FORMAT(b, 0x20);
}


static inline void set_status_cmd(unsigned char *b)
{
	SET_CMD_TYPE(b, 0x05);
}


static inline void set_write_reg_cmd(unsigned char *b, uint32_t addr, uint32_t v)
{
	SET_CMD_TYPE(b, 0x02);
	SET_ADDR(b, addr);
	SET_DATA(b, v);
	SET_FORMAT(b, 0x20);
	SET_COUNT(b, 4);
}


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


#define MAX_NUMBER_PROGS (0x380 - sizeof(syspage_t)) / sizeof(syspage_program_t)
/* may be 3c0 0x400 - 0x20 */

typedef struct {
	size_t alloc;
	size_t used;
	void *img_buf;
} image_t;


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


static void *read_file(char *path, size_t *sizeptr)
{
	int fd = -1;
	struct stat st;
	size_t offs, br;
	void *buf = NULL;

	if ((fd = open(path, O_RDONLY)) < 0) {
		fprintf(stderr, "File \"%s\" open error: %s\n", path, strerror(errno));
		return NULL;
	}

	if (fstat(fd, &st) != 0) {
		fprintf(stderr, "File \"%s\" stat error: %s\n", path, strerror(errno));
		goto err;
	}

	if ((buf = malloc(st.st_size)) == NULL) {
		fprintf(stderr, "Failed to allocate file data\n");
		goto err;
	}

	offs = 0;
	br = 1;
	while (br > 0 && offs < st.st_size) {
		br = read(fd, buf + offs, st.st_size - offs);
		offs += br;
	}

	if (br < 0) {
		fprintf(stderr, "File \"%s\" read error: %s\n", path, strerror(errno));
		goto err;
	}

	if (offs != st.st_size) {
		fprintf(stderr, "File \"%s\" read error: read %lu bytes instead of %lu.\n", path, offs, st.st_size);
		goto err;
	}

	close(fd);
	*sizeptr = st.st_size;
	return buf;

err:
	if (fd > 0)
		close(fd);
	free(buf);
	*sizeptr = 0;
	return NULL;
}


static mod_t *load_module(char *path)
{
	mod_t *mod;
	int i = 0;

	if (path[0] == 'X' || path[0] == 'F')
		i++;

	mod = malloc(sizeof(mod_t));
	mod->name = base_name(path);

	mod->data = read_file(path + i, &mod->size);
	if (mod->data == NULL) {
		free(mod->name);
		free(mod);
		mod = NULL;
	}

	return mod;
}


void print_progress(size_t sent, size_t all)
{
	fprintf(stderr, "\rSent (%lu/%lu) %5.2f%%     ", sent, all, ((float)sent / (float)all) * 100.0);
}


int send_close_command(hid_device *dev)
{
	int rc;
	unsigned char b[BUF_SIZE]={0};

	b[0] = 1;
	set_write_file_cmd(b + 1, 0, 0);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command (%d)\n", rc);
	}
	return rc;
}


int send_mod_name(hid_device *dev, mod_t *mod, uint32_t addr)
{
	int rc;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_write_file_cmd(b + 1, addr, strlen(mod->name) + 1);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command (%d)\n", rc);
		return rc;
	}

	/* Send name */
	b[0] = 2;
	memcpy(b + 1, mod->name, strlen(mod->name) + 1);
	if((rc = hid_write(dev, b, strlen(mod->name) + 2)) < 0) {
		fprintf(stderr, "\nFailed to send image name (%d)\n", rc);
		return rc;
	}
	return rc;
}


int send_mod_args(hid_device *dev, mod_t *mod, uint32_t addr)
{
	int rc;
	unsigned char b[BUF_SIZE]={0};

	int argsz = 0;
	if (mod->args != NULL) {
		argsz = strlen(mod->args) + 1;
	}

	if (argsz > 128) {
		mod->args[argsz - 1] = 0;
		fprintf(stderr, "Argument list is too long\n");
		argsz = 128;
	}

	/* Send write command */
	b[0] = 1;
	set_write_file_cmd(b + 1, addr, argsz);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command (%d)\n", rc);
		return rc;
	}

	if (argsz > 0) {
		/* Send arguments */
		b[0] = 2;
		memcpy(b + 1, mod->args, argsz);
		if((rc = hid_write(dev, b, argsz + 1)) < 0) {
			fprintf(stderr, "\nFailed to send image name (%d)\n", rc);
			return rc;
		}
	}
	return rc;
}


int send_mod_contents(hid_device *dev, mod_t *mod, uint32_t addr)
{
	int n,rc;
	ssize_t offset = 0;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_write_file_cmd(b + 1, addr, mod->size);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command (%d)\n", rc);
		return rc;
	}

	/* Send contents */
	b[0] = 2;
	int i = 0;
	while (offset < mod->size) {
		n = (BUF_SIZE - 1 > mod->size - offset) ? (mod->size - offset) : (BUF_SIZE - 1);
		memcpy(b + 1, mod->data + offset, n);
		offset += n;
		if ((i++ % 50) == 0) {
			print_progress(offset, mod->size);
		}
		if((rc = hid_write(dev, b, n + 1)) < 0) {
			print_progress(offset, mod->size);
			fprintf(stderr, "\nFailed to send image contents (%d)\n", rc);
			return rc;
		}
	}
	print_progress(offset, mod->size);
	printf("\n");
	return 0; // ignore report 3 and 4 for now

	//Receive report 3
	if ((rc = hid_read(dev, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		return rc;
	}
	//printf("HAB mode: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if ((rc = hid_read(dev, b, BUF_SIZE) < 0) || *(uint32_t*)(b + 1) != 0x88888888)
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);

	return rc;
}


int send_module(hid_device *dev, mod_t *mod, uint32_t addr)
{
	int rc = 0;
	if ((rc = send_mod_name(dev, mod, addr)) < 0) {
		return rc;
	}

	if ((rc = send_mod_args(dev, mod, addr)) < 0) {
		return rc;
	}

	rc = send_mod_contents(dev, mod, addr);
	return rc;
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


static int write_file(char *path, void *buf, size_t size)
{
	int fd;
	size_t n = size;
	ssize_t result;

	fd = open(path, O_RDWR | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		fprintf(stderr, "File \"%s\" create error: %s\n", path, strerror(errno));
		return -1;
	}

	while (n > 0) {
		result = write(fd, buf, n);
		if (result < 0) {
			fprintf(stderr, "File \"%s\" write error: %s\n", path, strerror(errno));
			break;
		}
		n -= result;
		buf += result;
	}
	close(fd);
	/* chmod(path, S_IRUSR | S_IWUSR); */
	return (n > 0) ? -1 : 0;
}

static int append_sysprogs(image_t *img_ptr, char *initrd, char *console, char *append, syspage_t *syspage, unsigned int addr)
{
	int i = 0, j = 0;
	char *progs, *prog;
	void *buf = NULL;;
	size_t size_prog;
	void *temp;
	size_t new_alloc;

	asprintf(&progs, "%s %s %s", console ? console : "", initrd ? initrd : "",  append ? append : "");
	prog = strtok(progs, " ");

	while (prog) {
		if (i >= MAX_NUMBER_PROGS) {
			fprintf(stderr, "Too many modules, max=%ld\n", MAX_NUMBER_PROGS);
			i = -1;
			break;
		}
		buf = read_file(prog, &size_prog);
		if (buf == NULL || size_prog == 0) {
			goto next;
		}

		/* check image size */
		if (img_ptr->alloc <= img_ptr->used + size_prog) {
			new_alloc = img_ptr->alloc * 2;
			temp = realloc(img_ptr->img_buf, new_alloc);
			if (temp == NULL) {
				fprintf(stderr, "Could not allocate %lu bytes for image\n", new_alloc);
				i = -1;
				break;
			}
			img_ptr->img_buf = temp;
			img_ptr->alloc = new_alloc;
		}

		syspage->progs[i].start = img_ptr->used + addr;
		memcpy(img_ptr->img_buf + img_ptr->used, buf, size_prog);
		img_ptr->used += size_prog;
		syspage->progs[i].end = img_ptr->used + addr;

		for (j = strlen(prog); j >= 0 && prog[j] != '/'; --j);

		strncpy(syspage->progs[i].cmdline, prog + j + 1, sizeof(syspage->progs[i].cmdline) - 1);
		syspage->progs[i].cmdline[sizeof(syspage->progs[i].cmdline) - 1] = '\0';

		printf("Processed \"%s\" (%u bytes)\n", prog, syspage->progs[i].end - syspage->progs[i].start);

		i++;

next:
		free(buf);
		prog = strtok(NULL, " ");
	}

	free(progs);
	return i;
}


int boot_image(char *kernel, char *initrd, char *console, char *append, char *output, int plugin)
{
	int err = 0;
	size_t cnt;
	uint32_t jump_addr, load_addr;
	char *arg = NULL;
	int plugin_sz = 0;
	syspage_t *syspage;
	int sysprogs_cnt;
	unsigned int addr = plugin ? ADDR_DDR : ADDR_OCRAM;

	image_t image = {0, 0, 0};
	void *buf;
	size_t size;

	kernel = strtok(kernel, "=");
	arg = strtok(NULL, "=");

	buf = read_file(kernel, &size);
	if (buf == NULL || size == 0) {
		return -1;
	}

	image.alloc = 1 << 18;
	while (size >= image.alloc) {
		image.alloc *= 2;
	}

	image.img_buf = malloc(image.alloc);
	if (image.img_buf == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for image\n", image.alloc);
		free(buf);
		return -1;
	}

	memcpy(image.img_buf, buf, size);
	image.used = size;
	free(buf);
	buf = NULL;

	jump_addr = *(uint32_t *)(image.img_buf + 0x400 + 20); //ivt self ptr
	load_addr = *(uint32_t *)(image.img_buf + 0x400 + 32); //ivt load address

	printf("Processed kernel image (%lu bytes)\n", image.used);

	if (image.used < (0x400 + 0x30)) {
		fprintf(stderr, "Kernel's too small\n");
		free(image.img_buf);
		return -1;
	}

	size = sizeof(syspage_t) + (MAX_NUMBER_PROGS * sizeof(syspage_program_t));
	syspage = calloc(size, 1);
	if (syspage == NULL) {
		fprintf(stderr, "Could not allocate %lu bytes for syspage\n", size);
		free(image.img_buf);
		return -1;
	}

	syspage->pbegin = PADDR_BEGIN;
	syspage->pend = PADDR_END;
	syspage->kernel = 0;
	syspage->kernelsize = image.used;
	syspage->console = 0;
	strncpy(syspage->arg, arg ? arg : "", sizeof(syspage->arg) - 1);
	syspage->arg[sizeof(syspage->arg) - 1] = '\0';
	syspage->progssz = 0;

	sysprogs_cnt = append_sysprogs(&image, initrd, console, append, syspage, addr);
	if (sysprogs_cnt < 0) {
		free(syspage);
		free(image.img_buf);
		return -1;
	}
	syspage->progssz = sysprogs_cnt;

	if (plugin) {
		plugin_sz = *(int *)(image.img_buf + 0x424);
		*(int *)(image.img_buf + 0x424) = (plugin_sz + 0x199) & ~0x1ff;

		if (((plugin_sz - 0xc) < 0) || ((plugin_sz - 0xc) > (image.used - sizeof(image.used)))) {
			fprintf(stderr, "Probably the kenel is damaged, plugin_sz=%d!\n", plugin_sz);
			free(syspage);
			free(image.img_buf);
			return -1;
		}

		memcpy(image.img_buf + plugin_sz - 0xc, &image.used, sizeof(image.used));
	} else
		memcpy(image.img_buf + 0x400 + 0x24, &image.used, sizeof(image.used));

	printf("Writing syspage...\n");

	cnt = sizeof(syspage_t) + (sysprogs_cnt * sizeof(syspage_program_t));
	memcpy(image.img_buf + 0x20, (void *)syspage, cnt);

	syspage_dump(syspage);

	free(syspage);

	printf("\nTotal image size: %lu bytes.\n\n", image.used);

	if (output == NULL) {
		if (plugin) {
			silent = 1;
			printf("Waiting for USB connection...");
			fflush(stdout);
			err = usb_vybrid_dispatch(NULL, (char *)&load_addr, (char *)&jump_addr, image.img_buf, plugin_sz);
			load_addr = 0x80000000;
			jump_addr = 0x80000000 + plugin_sz - 0x30;
			usleep(500000);
			silent = 0;
			printf("\r                              \r");
			fflush(stdout);
			if (err)
				goto out;
		}
		usb_vybrid_dispatch(NULL, (char *)&load_addr, (char *)&jump_addr, image.img_buf, image.used);
	} else {
		err = write_file(output, image.img_buf, image.used);
	}

out:
	free(image.img_buf);
	return err;
}


static hid_device* open_device_with_vid_pid(uint16_t vid, uint16_t pid)
{
	hid_device* h = NULL;
	struct hid_device_info* list = hid_enumerate(vid, pid); // Find all devices with given vendor

	for (struct hid_device_info* it = list; it != NULL; it = it->next) {
		if ((h = hid_open_path(it->path)) == NULL) {
			fprintf(stderr, "Failed to open device\n");
			continue;
		} else {
			break;
		}
	}

	if (list)
		hid_free_enumeration(list);

	return h;
}


int usb_imx_dispatch(char *kernel, char *console, char *initrd, char *append, int plugin)
{
	char *mod_tok, *arg_tok;
	char *mod_p, *arg_p;
	char *modules;
	mod_t *mod;
	hid_device *dev = NULL;
	int len = 3;

	if (boot_image(kernel, initrd, NULL, NULL, NULL, plugin)) {
		fprintf(stderr, "Image booting error. Exiting...\n");
		return -1;
	}

	printf("Waiting for the device to boot...");
	fflush(stdout);
	while ((dev = open_device_with_vid_pid(0x15a2, 0x007d)) == NULL);

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

	printf("modules: %s\n", modules);
	mod_tok = strtok_r(modules, " ", &mod_p);

	if (mod_tok == NULL)
		mod_tok = modules;

	while (mod_tok != NULL) {

		arg_tok = strtok_r(mod_tok, "=", &arg_p);
		if ((mod = load_module(arg_tok)) == NULL) {
			send_close_command(dev);
			hid_close(dev);
			hid_exit();
			free(modules);
			return 1;
		}

		mod->args = strtok_r(NULL, " ", &arg_p);
		printf("Sending module '%s'\n", mod->name + 1);
		if (send_module(dev, mod, 0)) {
			send_close_command(dev);
			hid_close(dev);
			hid_exit();
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

	send_close_command(dev);
	hid_close(dev);
	hid_exit();
	free(modules);
	printf("Transfer complete\n");
	return 0;
}
