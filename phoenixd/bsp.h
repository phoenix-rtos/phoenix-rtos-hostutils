/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * BSP protocol implementation
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2001 Pawel Pisarczyk
 * Author: Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _BSP_H_
#define _BSP_H_


#include <hostutils-common/types.h>


/* BSP sizes */
#define BSP_HDRSZ        3
#define BSP_MSGSZ        1024
#define BSP_FRAMESZ      BSP_MSGSZ * 2 + BSP_HDRSZ


/* BSP characters */
#define BSP_ESCCHAR   (u8)0xaa
#define BSP_ENDCHAR   (u8)0xdd


/* BSP frame types */
#define BSP_TYPE_ACK       1
#define BSP_TYPE_RETR      2
#define BSP_TYPE_KREQ      3
#define BSP_TYPE_SHDR      4
#define BSP_TYPE_KDATA     5
#define BSP_TYPE_GO        6
#define BSP_TYPE_PDATA     7
#define BSP_TYPE_EHDR      8
#define BSP_TYPE_PHDR      9
#define BSP_TYPE_ERR       10


/* BSP timings */
#define BSP_INF        0
#define BSP_TIMEOUT    3 * 1000
#define BSP_MAXREP     3


/* Function sends BSP message */
extern int bsp_send(int fd, u8 t, char *buffer, uint len);


/* Function receives BSP message */
extern int bsp_recv(int fd, u8 *t, char *in_buffer, uint len, uint timeout);


/* Function sends BSP request (sends message and waits for answer) */
extern int bsp_req(int fd, u8 st, char *sbuff, uint slen, u8 *rt, u8 *rbuff, uint rlen, u16 num, u16 *rnum);


/* Functions sends kernel to Phoenix node */
extern int bsp_sendkernel(int fd, char *kernel);


/* Function sends user program to Phoenix node */
extern int bsp_sendprogram(int fd, char *name, char *sysdir);


#endif
