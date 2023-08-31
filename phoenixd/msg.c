/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP2 protocol implementation
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2004 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <hostutils-common/errors.h>
#include <hostutils-common/serial.h>
#include "msg.h"


u32 msg_csum(msg_t *msg)
{
	unsigned int k;
	u16 csum;

	csum = 0;
	for (k = 0; k < MSG_HDRSZ + msg_getlen(msg); k++) {
		if (k >= sizeof(msg->csum)) {
			csum += *((u8 *)msg + k);
		}
	}
	csum += msg_getseq(msg);
	return csum;
}


int msg_serial_send(int fd, msg_t *msg, u16 seq)
{
	u8 *p = (u8 *)msg;
	u8 cs[2];
	unsigned int k;
	u8 buff[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
	unsigned int i = 0;

	msg_setseq(msg, seq);
	msg_setcsum(msg, msg_csum(msg));
	cs[0] = MSG_MARK;

	if (msg_getlen(msg) > MSG_MAXLEN)
		return ERR_MSG_ARG;

	buff[i++] = cs[0];

	for (k = 0; k < MSG_HDRSZ + msg_getlen(msg); k++) {

		if ((p[k] == MSG_MARK) || (p[k] == MSG_ESC)) {
			cs[0] = MSG_ESC;
			if (p[k] == MSG_MARK)
				cs[1] = MSG_ESCMARK;
			else
				cs[1] = MSG_ESCESC;
			memcpy(&buff[i], cs, 2);
			i += 2;
		}
		else
			buff[i++] = p[k];
	}
	if (serial_write(fd, buff, i) < 0)
		return ERR_MSG_IO;

	return k;
}


int msg_serial_recv(int fd, msg_t *msg, int *state)
{
	int escfl = 0;
	unsigned int l = 0;
	u8 c;

	for (;;) {
		if (serial_read(fd, &c, 1, 0) < 0) {
			*state = MSGRECV_DESYN;
			return ERR_MSG_IO;
		}

		if (*state == MSGRECV_FRAME) {

			/* Return error if frame is to long */
			if (l == MSG_HDRSZ + MSG_MAXLEN) {
				*state = MSGRECV_DESYN;
				return ERR_MSG_IO;
			}

			/* Return error if terminator discovered */
			if (c == MSG_MARK) {
				return ERR_MSG_IO;
			}

			if (!escfl && (c == MSG_ESC)) {
				escfl = 1;
				continue;
			}
			if (escfl) {
				if (c == MSG_ESCMARK)
					c = MSG_MARK;
				if (c == MSG_ESCESC)
					c = MSG_ESC;
				escfl = 0;
			}
			*((u8 *)msg + l++) = c;

			/* Frame received */
			if ((l >= MSG_HDRSZ) && (l == msg_getlen(msg) + MSG_HDRSZ)) {
				*state = MSGRECV_DESYN;
				break;
			}
		}
		else {
			/* Synchronize */
			if (c == MSG_MARK)
				*state = MSGRECV_FRAME;
		}
	}

	/* Verify received message */
	//if (msg->csum != msg_csum(msg)) {
	//	return ERR_MSG_IO;
	//}

	return l;
}
