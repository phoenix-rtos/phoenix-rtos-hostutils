/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP2 protocol tunneled over TCP connection
 *
 * Copyright 2023 Phoenix Systems
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <hostutils-common/errors.h>
#include "msg_tcp.h"


extern u32 msg_csum(msg_t *msg);


int tcp_open(char *addrstr, unsigned int port)
{
	struct sockaddr_in server;
	int sock;

	size_t cfgLen = 0;
	const char *cfgString = getenv("PHOENIXD_TCP");

	if (cfgString != NULL) {
		cfgLen = strlen(cfgString);
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("Could not create socket");
		return -1;
	}

	server.sin_addr.s_addr = inet_addr(addrstr);
	server.sin_family = AF_INET;
	server.sin_port = htons(port);

	if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
		perror("Connect failed");
		return -1;
	}

	/* Send optional tunnel configuration string */
	if ((cfgLen > 0) && (send(sock, cfgString, cfgLen, 0) <= 0)) {
		perror("Failed to send configuration");
		close(sock);
		return -1;
	}

	return sock;
}


int msg_tcp_send(int fd, msg_t *msg, u16 seq)
{
	unsigned char buf[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
	unsigned char *end = (unsigned char *)msg;
	unsigned char *p = (unsigned char *)msg;
	size_t i = 0;

	msg_setseq(msg, seq);
	msg_setcsum(msg, msg_csum(msg));
	end += MSG_HDRSZ;

	if (msg_getlen(msg) >= MSG_MAXLEN) {
		return ERR_MSG_ARG;
	}

	buf[i++] = MSG_MARK;

	end += msg_getlen(msg);
	for (; p < end; p++) {
		if ((*p == MSG_MARK) || (*p == MSG_ESC)) {
			buf[i++] = MSG_ESC;
			if (*p == MSG_MARK) {
				buf[i++] = MSG_ESCMARK;
			}
			else {
				buf[i++] = MSG_ESCESC;
			}
		}
		else {
			buf[i++] = *p;
		}
	}

	if (send(fd, buf, i, 0) < 0) {
		return ERR_MSG_IO;
	}

	return (int)(end - (unsigned char *)msg);
}


int msg_tcp_recv(int fd, msg_t *msg, int *state)
{
	ssize_t r;
	unsigned char c;
	unsigned char *buf = (unsigned char *)msg;
	int l = 0;
	int escfl = 0;

	for (;;) {
		r = recv(fd, &c, 1, 0);
		if (r == 0) {
			*state = MSGRECV_DESYN;
			return ERR_MSG_CLOSED;
		}
		else if (r <= 0) {
			*state = MSGRECV_DESYN;
			return ERR_MSG_IO;
		}
		else if (*state == MSGRECV_FRAME) {
			/* Return error if frame is to long */
			if (l == MSG_HDRSZ + MSG_MAXLEN) {
				*state = MSGRECV_DESYN;
				return ERR_MSG_IO;
			}

			/* Return error if terminator discovered */
			if (c == MSG_MARK) {
				return ERR_MSG_IO;
			}

			if ((escfl == 0) && (c == MSG_ESC)) {
				escfl = 1;
				continue;
			}

			if (escfl != 0) {
				escfl = 0;
				switch (c) {
					case MSG_ESCMARK: c = MSG_MARK; break;
					case MSG_ESCESC: c = MSG_ESC; break;
					default: break;
				}
			}
			buf[l++] = c;

			/* Frame received */
			if ((l >= MSG_HDRSZ) && (l == msg_getlen(msg) + MSG_HDRSZ)) {
				*state = MSGRECV_DESYN;
				break;
			}
		}
		else {
			/* Synchronize */
			if (c == MSG_MARK) {
				*state = MSGRECV_FRAME;
			}
		}
	}

	return l;
}
