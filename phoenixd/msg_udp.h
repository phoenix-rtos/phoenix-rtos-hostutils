/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP2 protocol implementation
 *
 * Copyright 2012 Phoenix Systems
 * Author Jacek Popko
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _MSG_UDP_H_
#define _MSG_UDP_H_

#include <hostutils-common/types.h>
#include "msg.h"

#define PHFS_UDPPORT 11520

extern int udp_open(char *node, uint port);
extern int msg_udp_send(int fd, msg_t *msg, u16 seq);
extern int msg_udp_recv(int fd, msg_t *msg, int *state);

#endif
