/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP2 message dispatcher
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2004 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _DISPATCH_H_
#define _DISPATCH_H_
#include "msg.h"

#include <termio.h>


typedef enum {
	SERIAL,
	PIPE,
	UDP,
	USB_VYBRID,
	USB_IMX
} dmode_t;


/* Function reads and dispatches messages */
extern int dispatch(char *dev_addr, dmode_t mode, char *sysdir, void *data);
extern int (*msg_send)(int fd, msg_t *msg, u16 seq);
extern int (*msg_recv)(int fd, msg_t *msg, int *state);

extern int usb_vybrid_dispatch(char* kernel, char* loadAddr, char* jump_addr, void *image, ssize_t size);

extern int usb_imx_dispatch(char *kernel, char *uart, char *initrd, char *append, int plugin);
extern int boot_image(char *kernel, char *initrd, char *console, char *append, char *output, int plugin);


#endif
