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

#ifndef _COMMON_DISPATCH_H_
#define _COMMON_DISPATCH_H_
#include <stdint.h>

extern int usb_vybrid_dispatch(char *kernel, char *loadAddr, char *jump_addr, void *image, ssize_t size);

extern int usb_imx_dispatch(char *kernel, char *uart, char *initrd, char *append, int plugin);


#endif
