/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP2 protocol tunneled over TCP connection
 *
 * Copyright 2023 Phoenix Systems
 * Author Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _MSG_TCP_H_
#define _MSG_TCP_H_

#include <hostutils-common/types.h>
#include "msg.h"

#define PHFS_TCPPORT 18022

extern int tcp_open(char *node, uint port);
extern int msg_tcp_send(int fd, msg_t *msg, u16 seq);
extern int msg_tcp_recv(int fd, msg_t *msg, int *state);

#endif
