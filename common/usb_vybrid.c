/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * load modules for Vybrid
 *
 * Copyright 2014, 2018 Phoenix Systems
 * Author: Kamil Amanowicz, Pawel Tryfon
 *
 * This file is part of Phoenix-RTOS.
 *
 * See the LICENSE
 */


#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include <hidapi/hidapi.h>

#include "hostutils-common/dispatch.h"

/* SDP protocol section */
#define SET_CMD_TYPE(b,v) (b)[0]=(b)[1]=(v)
#define SET_ADDR(b,v) *((uint32_t*)((b)+2))=htonl(v)
#define SET_COUNT(b,v) *((uint32_t*)((b)+7))=htonl(v);
#define SET_DATA(b,v) *((uint32_t*)((b)+11))=htonl(v);
#define SET_FORMAT(b,v) (b)[6]=(v);

int silent = 0;

#define dispatch_msg(silent, fmt, ...)		\
	do {								\
		if (!silent)					\
			printf(fmt, ##__VA_ARGS__);		\
	} while(0)


static inline void set_write_file_cmd(unsigned char *b, uint32_t addr, uint32_t size)
{
	SET_CMD_TYPE(b,0x04);
	SET_ADDR(b,addr);
	SET_COUNT(b,size);
	SET_FORMAT(b,0x20);
}


static inline void set_jmp_cmd(unsigned char *b, uint32_t addr)
{
	SET_CMD_TYPE(b,0x0b);
	SET_ADDR(b,addr);
	SET_FORMAT(b,0x20);
}


static inline void set_status_cmd(unsigned char *b)
{
	SET_CMD_TYPE(b,0x05);
}


static inline void set_write_reg_cmd(unsigned char *b, uint32_t addr, uint32_t v)
{
	SET_CMD_TYPE(b,0x02);
	SET_ADDR(b,addr);
	SET_DATA(b,v);
	SET_FORMAT(b,0x20);
	SET_COUNT(b,4);
}


void print_cmd(unsigned char* b)
{
	printf("Command:\n  type=%02x%02x, addr=%08x, format=%02x, count=%08x, data=%08x\n",b[0],b[1],*(uint32_t*)(b+2),b[6],*(uint32_t*)(b+7),*(uint32_t*)(b+11));

}


static int open_vybrid(hid_device** h)
{
	int retval = 0;
	struct hid_device_info* list = hid_enumerate(0x15a2, 0x0); // Find all devices with given vendor

	for (struct hid_device_info* it = list; it != NULL; it = it->next) {
		if ((it->product_id == 0x0080) || (it->product_id == 0x007d) || (it->product_id == 0x006a)) {
			dispatch_msg(silent, "Found supported device\n");
		} else {
			printf("Found unsuported product of known vendor, trying standard settings for this device\n");
		}

		if ((*h = hid_open_path(it->path)) == NULL) {
			fprintf(stderr, "Failed to open device\n");
			continue;
		} else {
			retval = 1;
			break;
		}
	}

	if (list)
		hid_free_enumeration(list);

	return retval;
}


#define CMD_SIZE 17
#define BUF_SIZE 1025
#define INTERRUPT_SIZE 65
int load_file(hid_device *h, char *filename, uint32_t addr)
{
	int fd = -1, n, rc;
	struct stat f_st;
	unsigned char b[BUF_SIZE] = {0};

	if ((fd = open(filename,O_RDONLY)) < 0) {
		fprintf(stderr,"Failed to open file (%s)\n", strerror(errno));
		return -1;
	}

	if(fstat(fd, &f_st) != 0) {
		fprintf(stderr,"File stat failed (%s)\n", strerror(errno));
		close(fd);
		return -1;
	}

	b[0] = 1;
	set_write_file_cmd(b + 1, addr, f_st.st_size);
	//print_cmd(b+1);
	if ((rc = hid_write(h, b, CMD_SIZE)) < 0){
		fprintf(stderr, "Failed to send write_file command\n");
		goto END;
	}

	b[0] = 2;
	while((n = read(fd, b + 1, BUF_SIZE - 1)) > 0)
		if((rc = hid_write(h, b, n + 1)) < 0) {
			fprintf(stderr, "Failed to send file contents\n");
			goto END;
		}

	if(n < 0) {
		fprintf(stderr, "Error reading file (%d,%s)\n", n, strerror(errno));
		rc = -1;
		goto END;
	}

	//Receive report 3
	if((rc = hid_read(h, b, BUF_SIZE)) != 5) {
		fprintf(stderr,"Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		goto END;
	}
	//printf("HAB mode: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if(((rc = hid_read(h, b, BUF_SIZE)) < 0) || *(uint32_t *)(b + 1) != 0x88888888) {
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);
		goto END;
	}

END:
	if(fd > -1)
		close(fd);
	return rc;
}


int load_image(hid_device *h, void *image, ssize_t size, uint32_t addr)
{
	int n, rc;
	ssize_t offset = 0;
	unsigned char b[BUF_SIZE] = {0};

	b[0] = 1;
	set_write_file_cmd(b + 1, addr, size);
	//print_cmd(b+1);
	if ((rc = hid_write(h, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command\n");
		goto END;
	}

	b[0] = 2;
	while (offset < size) {
		n = (BUF_SIZE - 1 > size - offset) ? (size - offset) : (BUF_SIZE - 1);
		memcpy(b + 1, image + offset, n);
		offset += n;
		if((rc = hid_write(h, b, n + 1)) < 0) {
			fprintf(stderr, "Failed to send image contents\n");
			goto END;
		}
	}

	//Receive report 3
	if ((rc = hid_read(h, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		goto END;
	}
	//printf("HAB mode: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if ((rc = hid_read(h, b, BUF_SIZE) < 0) || *(uint32_t *)(b + 1) != 0x88888888)
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);

END:
	return rc;
}


int jmp_2_addr(hid_device *h, uint32_t addr)
{
	int rc = 0;
	unsigned char b[INTERRUPT_SIZE] = {0};

	b[0] = 1;
	set_jmp_cmd(b + 1, addr);
	//print_cmd(b+1);
	if((rc = hid_write(h, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send jmp command");
		goto END;
	}
	if((rc = hid_read(h, b, INTERRUPT_SIZE)) < 0) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)", rc);
		goto END;
	}
	//printf("HAB: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if((rc = hid_read(h, b, INTERRUPT_SIZE)) >= 0) {
		fprintf(stderr, "Received HAB error status (n=%d): %02x%02x%02x%02x\nJump address command failed\n", rc, b[1], b[2], b[3], b[4]);
		goto END;
	} else
		rc = 0;

END:
	return rc;
}


int write_reg(hid_device *h, uint32_t addr, uint32_t v)
{
	int rc;
	unsigned char b[INTERRUPT_SIZE] = {0};

	b[0] = 1;
	set_write_reg_cmd(b + 1, addr, v);
	//print_cmd(b+1);
	rc = hid_write(h, b, CMD_SIZE);
	if(rc < 0)
		fprintf(stderr, "Failed to send write command");
	else
		rc = hid_read(h, b, INTERRUPT_SIZE);
	if(rc != 5)
		fprintf(stderr, "Failed to receive HAB mode (n=%d)", rc);
	else
		rc = hid_read(h, b, INTERRUPT_SIZE);
	if(rc < 0)
		fprintf(stderr, "Failed to receive status (n=%d)", rc);
	//printf("Status: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);

	return rc;
}


int do_status(hid_device *h)
{
	unsigned char b[INTERRUPT_SIZE] = {0};
	int rc;

	b[0] = 1;
	set_status_cmd(b + 1);
	//print_cmd(b+1);
	if (silent)
		fprintf(stderr, "\n");

	if((rc = hid_write(h, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send status command (%d)\n", rc);
		goto END;
	}
	if((rc = hid_read(h, b, INTERRUPT_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		goto END;
	}
	if((rc = hid_read(h, b, INTERRUPT_SIZE)) < 0) {
		fprintf(stderr, "Failed to receive status (n=%d)\n", rc);
		goto END;
	}
	//printf("Status: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);

END:
	return rc < 0;
}


int usb_vybrid_dispatch(char *kernel, char *loadAddr, char *jumpAddr, void *image, ssize_t size)
{
	int rc;
	int err = 0;
	hid_device *h = 0;

	hid_init();

	dispatch_msg(silent, "Starting usb loader.\nWaiting for compatible USB device to be discovered ...\n");
	while(1){
		if (err) {
			usleep(500000);
			if (err > 5)
				return -1;
		}
		err++;

		if(open_vybrid(&h) == 0) {
			if (err)
				err--;
			continue;
		}

		if((rc = do_status(h)) != 0) {
			fprintf(stderr, "Device failure (check if device is in serial download mode, check USB connection)\n");
			return -1;
		}

		uint32_t load_addr = 0;
		if (kernel != NULL && image == NULL && size == 0) {
			if (loadAddr != NULL)
				load_addr = strtoul(loadAddr, NULL, 16);
			if (load_addr == 0)
				load_addr = 0x3f000000;

			if((rc = load_file(h, kernel, load_addr)) != 0) {
				fprintf(stderr, "Failed to load file to device\n");
				continue;
			}
		} else {
			if (loadAddr != NULL)
				load_addr = *(uint32_t *)loadAddr;
			if (load_addr == 0)
				load_addr = 0x3f000000;
			if ((rc = load_image(h, image, size, load_addr)) != 0) {
				fprintf(stderr, "Failed to load image to device\n");
				continue;
			}
		}
		dispatch_msg(silent, "Image file loaded.\n");

		uint32_t jump_addr = 0;
		if (kernel != NULL && image == NULL && size == 0) {
			if(jumpAddr != NULL)
				jump_addr = strtoul(jumpAddr,NULL,16);
		} else {
			if(jumpAddr != NULL)
				jump_addr = *(uint32_t *)jumpAddr;
		}
		if(jump_addr == 0)
			jump_addr = 0x3f000400;
		if((rc = jmp_2_addr(h,jump_addr)) != 0) {
			fprintf(stderr, "Failed to send jump command to device (%d)\n",rc);
			continue;
		}
		dispatch_msg(silent, "Code execution started.\n");

		break;
	}

	dispatch_msg(silent, "Closing usb loader\n");
	if (h)
		hid_close(h);
	hid_exit();
	return rc;
}
