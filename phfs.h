/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Phoenix remote filesystem server
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

#ifndef _PHFS_H_
#define _PHFS_H_


#define MSG_OPEN   1
#define MSG_READ   2
#define MSG_WRITE  3
#define MSG_CLOSE  4


#define PHFS_RDONLY  0
#define PHFS_RDWR    1


typedef struct _msg_phfsio_t {
	u32 handle;
	u32 pos;
	u32 len;
	u8  buff[MSG_MAXLEN - 3 * sizeof(u32)];
} msg_phfsio_t;


extern int phfs_handlemsg(int fd, msg_t *msg, char *sysdir);


#endif
