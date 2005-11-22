/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * BSP2 protocol implementation
 *
 * Copyright 2004 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * Phoenix-RTOS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phoenix-RTOS kernel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phoenix-RTOS kernel; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "errors.h"
#include "serial.h"
#include "msg.h"


u32 msg_csum(msg_t *msg)
{
	unsigned int k;
	u32 csum;
	
	csum = 0;
	for (k = 0; k < MSG_HDRSZ + msg_getlen(msg); k++) {
		if (k >= sizeof(msg->csum))
			csum += *((u8 *)msg + k);
	}
	return csum;
}


int msg_send(int fd, msg_t *msg)
{
	u8 *p = (u8 *)msg;
	u8 cs[2];
	unsigned int k;
	u8 buff[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
	unsigned int i = 0;
	
	msg->csum = msg_csum(msg);
	cs[0] = MSG_MARK;
	
	if (msg_getlen(msg) >= MSG_MAXLEN)
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


int msg_recv(int fd, msg_t *msg, int *state)
{
	int escfl = 0;
	unsigned int l = 0;
	u8 c;
	
	for (;;) {
		if (serial_read(fd, &c, 1, 0) < 0) {
			*state = MSGRECV_DESYN;
printf("timeout\n");
			return ERR_MSG_IO;
		}
			
		if (*state == MSGRECV_FRAME) {
			
			/* Return error if frame is to long */
			if (l == MSG_HDRSZ + MSG_MAXLEN) {
				*state = MSGRECV_DESYN;
printf("frame to long\n");
				return ERR_MSG_IO;
			}
				
			/* Return error if terminator discovered */
			if (c == MSG_MARK) {
printf("terminator discovered\n");
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
	if (msg->csum != msg_csum(msg)) {
printf("checksum\n");
		return ERR_MSG_IO;
	}

	return l;
}
