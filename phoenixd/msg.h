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

#ifndef _MSG_H_
#define _MSG_H_

#include <hostutils-common/types.h>


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
#define msg_settype(m, t)  ((m)->type = ((m)->type & ~0xffff) | ((t) & 0xffff))
#define msg_gettype(m)     ((m)->type & 0xffff)

#define msg_setlen(m, l)   ((m)->type = ((m)->type & 0xffff) | ((l) << 16))
#define msg_getlen(m)      ((m)->type >> 16)

#define msg_setcsum(m, c)  ((m)->csum = ((m)->csum & ~0xffff) | ((c) & 0xffff))
#define msg_getcsum(m)     ((m)->csum & 0xffff)

#define msg_setseq(m, s)   ((m)->csum = ((m)->csum & 0xffff) | ((s) << 16))
#define msg_getseq(m)      ((m)->csum >> 16)

extern int msg_serial_send(int fd, msg_t *msg, u16 seq);

extern int msg_serial_recv(int fd, msg_t *msg, int *state);


#endif
