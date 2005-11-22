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

#ifndef _MSG_H_
#define _MSG_H_

#include "types.h"


/* Special characters */
#define MSG_MARK     0x7e
#define MSG_ESC      0x7d
#define MSG_ESCMARK  0x5e
#define MSG_ESCESC   0x5d


/* Receive states */
#define MSGRECV_DESYN   0
#define MSGRECV_FRAME   1


/* Message types */
#define MSG_ERR      0


#define MSG_HDRSZ   2 * sizeof(u32)
#define MSG_MAXLEN  512


typedef struct _msg_t {
	u32 csum;
	u32 type;
	u8  data[MSG_MAXLEN];
} msg_t;


/* Macros for modifying message headers */
#define msg_settype(m, t)  ((m)->type = ((m)->type & ~0xffff) | (t & 0xffff))
#define msg_gettype(m)     ((m)->type & 0xffff)

#define msg_setlen(m, l)   ((m)->type = ((m)->type & 0xffff) | ((l) << 16))
#define msg_getlen(m)      ((m)->type >> 16)


extern int msg_send(int fd, msg_t *msg);

extern int msg_recv(int fd, msg_t *msg, int *state);


#endif
