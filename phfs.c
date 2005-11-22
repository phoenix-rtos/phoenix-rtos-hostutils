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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "errors.h"
#include "msg.h"
#include "phfs.h"


int phfs_open(int fd, msg_t *msg, char *sysdir)
{
	char *path = &msg->data[sizeof(u32)];
	int flags = *(u32 *)msg->data;
	int f = 0;
	char *realpath;
	int ofd;
	
	msg->data[MSG_MAXLEN] = 0;
		
	f =  flags == PHFS_RDONLY ? O_RDONLY : O_RDWR;
	msg_settype(msg, MSG_OPEN);
	msg_setlen(msg, sizeof(int));
	
	if ((realpath = malloc(strlen(sysdir) + 1 + strlen(path) + 1)) == NULL)
		*(u32 *)msg->data = 0;
	else {
		sprintf(realpath, "%s/%s", sysdir, path);
		ofd = open(realpath, f);	
		printf("[%d] phfs: MSG_OPEN %s [%s] ofs=%d\n", getpid(), path, realpath, ofd);
		*(u32 *)msg->data = ofd > 0 ? ofd : 0;
		free(realpath);
	}
	
	if (msg_send(fd, msg) < 0)
		return ERR_PHFS_IO;
	return 1;
}


int phfs_read(int fd, msg_t *msg, char *sysdir)
{
	msg_phfsio_t *io = (msg_phfsio_t *)msg->data;
	u32 hdrsz;
	u32 l;

	hdrsz = (u32)((u8 *)io->buff - (u8 *)io);
	
	if (io->len > MSG_MAXLEN - hdrsz)
		io->len = MSG_MAXLEN - hdrsz;
	
	lseek(io->handle, io->pos, SEEK_SET);
	io->len = read(io->handle, io->buff, io->len);
	
	l =  io->len > 0 ? io->len : 0;
	io->pos += l;
	
	msg_settype(msg, MSG_READ);
	msg_setlen(msg, l + hdrsz);

	if (msg_send(fd, msg) < 0)
		return ERR_PHFS_IO;

	return 1;
}


int phfs_write(int fd, msg_t *msg, char *sysdir)
{
	msg_phfsio_t *io = (msg_phfsio_t *)msg->data;
	u32 hdrsz;
	u32 l;
	
	hdrsz = (u32)((u8 *)io->buff - (u8 *)io);
	
	if (io->len > MSG_MAXLEN - hdrsz)
		io->len = MSG_MAXLEN - hdrsz;
	
	lseek(io->handle, io->pos, SEEK_SET);
	io->len = write(io->handle, io->buff, io->len);
	
	l =  io->len > 0 ? io->len : 0;
	io->pos += l;
	
	msg_settype(msg, MSG_READ);
	msg_setlen(msg, l + hdrsz);
	
	if (msg_send(fd, msg) < 0)
		return ERR_PHFS_IO;

	return 1;
}


int phfs_close(int fd, msg_t *msg, char *sysdir)
{
	int ofd = *(int *)msg->data;
	
	close(ofd);
	msg_settype(msg, MSG_CLOSE);
	msg_setlen(msg, 0);
	
	if (msg_send(fd, msg) < 0)
		return ERR_PHFS_IO;
	return 1;
}


int phfs_handlemsg(int fd, msg_t *msg, char *sysdir)
{
	int res = 0;
	
	switch (msg_gettype(msg)) {
	case MSG_OPEN:
		res = phfs_open(fd, msg, sysdir);
		break;
	case MSG_READ:
		res = phfs_read(fd, msg, sysdir);
		break;
	case MSG_WRITE:
		res = phfs_write(fd, msg, sysdir);
		break;
	case MSG_CLOSE:
		res = phfs_close(fd, msg, sysdir);
		break;
	}
	return res;
}
