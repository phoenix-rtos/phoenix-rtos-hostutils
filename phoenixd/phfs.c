/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * Phoenix remote filesystem server
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2004 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <hostutils-common/errors.h>
#include "dispatch.h"
#include "msg.h"
#include "phfs.h"


int phfs_open(int fd, msg_t *msg, char *sysdir)
{
	char *path = (char *)&msg->data[sizeof(u32)], *realpath;
	int flags = *(u32 *)msg->data, f = 0, ofd;
	u16 seq = msg_getseq(msg);

	msg->data[MSG_MAXLEN - 1] = 0;

	f = ((flags & 0x1) == PHFS_RDONLY) ? O_RDONLY : O_RDWR;
	f = ((flags & 0x2) == PHFS_CREATE) ? (f | O_CREAT) : f;

	msg_settype(msg, MSG_OPEN);
	msg_setlen(msg, sizeof(int));

	if ((realpath = malloc(strlen(sysdir) + 1 + strlen(path) + 1)) == NULL)
		*(u32 *)msg->data = 0;
	else {
		sprintf(realpath, "%s/%s", sysdir, path);

		if (flags == PHFS_RDONLY)
			ofd = open(realpath, f);
		else
			ofd = open(realpath, f, S_IRUSR | S_IWUSR);

		printf("[%d] phfs: %s path='%s', realpath='%s', ofd=%d\n", getpid(), ((f & O_CREAT) == O_CREAT) ? "MSG_CREATE" : "MSG_OPEN", path, realpath, ofd);
		*(u32 *)msg->data = ofd > 0 ? ofd : 0;
		free(realpath);
	}

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;
	return 1;
}


int phfs_read(int fd, msg_t *msg, char *sysdir)
{
	msg_phfsio_t *io = (msg_phfsio_t *)msg->data;
	u16 seq = msg_getseq(msg);
	u32 hdrsz;
	u32 l, pos, len;

	hdrsz = (u32)((u8 *)io->buff - (u8 *)io);
	if (io->len > MSG_MAXLEN - hdrsz)
		io->len = MSG_MAXLEN - hdrsz;

	len = io->len;
	pos = io->pos;
	lseek(io->handle, io->pos, SEEK_SET);
	io->len = read(io->handle, io->buff, io->len);

	l = (io->len > 0) ? io->len : 0;
	io->pos += l;

	printf("[%d] phfs: MSG_READ ofd=%d, pos=%d, len=%d, ret=%d\n",
		getpid(), io->handle, pos, len, io->len);

	msg_settype(msg, MSG_READ);
	msg_setlen(msg, l + hdrsz);

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;

	return 1;
}


int phfs_write(int fd, msg_t *msg, char *sysdir)
{
	msg_phfsio_t *io = (msg_phfsio_t *)msg->data;
	u32 hdrsz, l;
	u16 seq = msg_getseq(msg);

	hdrsz = (u32)((u8 *)io->buff - (u8 *)io);

	if (io->len > MSG_MAXLEN - hdrsz)
		io->len = MSG_MAXLEN - hdrsz;

	lseek(io->handle, io->pos, SEEK_SET);
	io->len = write(io->handle, io->buff, io->len);

	printf("[%d] phfs: MSG_WRITE fd=%d, pos=%d, ret=%d\n",
		getpid(), io->handle, io->pos, io->len);

	l = (io->len > 0) ? io->len : 0;
	io->pos += l;

	msg_settype(msg, MSG_WRITE);
	msg_setlen(msg, l + hdrsz);

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;

	return 1;
}


int phfs_close(int fd, msg_t *msg, char *sysdir)
{
	int ofd = *(int *)msg->data;
	u16 seq = msg_getseq(msg);

	printf("[%d] phfs: MSG_CLOSE ofd=%d\n", getpid(), ofd);
	close(ofd);
	msg_settype(msg, MSG_CLOSE);
	msg_setlen(msg, sizeof(int));

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;
	return 1;
}


int phfs_reset(int fd, msg_t *msg, char *sysdir)
{
	int i;
	struct rlimit rlim;
	u16 seq = msg_getseq(msg);

	printf("[%d] phfs: MSG_RESET\n", getpid());
	getrlimit(RLIMIT_NOFILE, &rlim);
	for (i = 3; i < rlim.rlim_cur; i++) {
		if (i != fd)
			close(i);
	}

	msg_settype(msg, MSG_RESET);
	msg_setlen(msg, 0);

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;
	return 1;
}


int phfs_stat(int fd, msg_t *msg, char *sysdir)
{
	msg_phfsio_t *io = (msg_phfsio_t *)msg->data;
	u16 seq = msg_getseq(msg);
	u32 hdrsz;
	u32 l;
	hdrsz = (u32)((u8 *)io->buff - (u8 *)io);
	if (io->len > MSG_MAXLEN - hdrsz)
		io->len = MSG_MAXLEN - hdrsz;

	struct pho_stat stat_send, test;
	struct stat st;

	fstat(io->handle, &st);

	stat_send.st_dev = st.st_dev;
	stat_send.st_ino = st.st_ino;
	stat_send.st_mode = st.st_mode;
	stat_send.st_nlink = st.st_nlink;
	stat_send.st_uid = st.st_uid;
	stat_send.st_gid = st.st_gid;
	stat_send.st_rdev = st.st_rdev;
	stat_send.st_size = st.st_size;

	stat_send.st_atime_ = st.st_atime;
	stat_send.st_mtime_ = st.st_mtime;
	stat_send.st_ctime_ = st.st_ctime;
	stat_send.st_blksize = st.st_blksize;
	stat_send.st_blocks = st.st_blocks;

	memcpy(io->buff, &stat_send, sizeof(stat_send));
	memcpy(&test, io->buff, sizeof(stat_send));
	io->pos = 0;
	l = sizeof(stat_send);
	msg->data[MSG_MAXLEN - 1] = 0;
	io->len = l;
	msg_settype(msg, MSG_FSTAT);
	msg_setlen(msg, l + hdrsz);

	printf("[%d] phfs: MSG_STAT id:%d  \n", getpid(), io->handle);

	if (msg_send(fd, msg, seq) < 0)
		return ERR_PHFS_IO;
	return 1;
}

#if 0
int phfs_lookup(int fd, msg_t *msg, char *sysdir)
{
	char *name; ;
	int flags = *(u32 *)msg->data, f = 0, ofd;

	msg->data[MSG_MAXLEN] = 0;

	name = &msg->data[sizeof(u32)];

	msg_settype(msg, MSG_LOOKUP);
	msg_setlen(msg, sizeof(int));

	if ((entry = cache_find(id)) == NULL) {
		vnode = vnode_get(id);
		if (id == 0)
			vnode->path = sysdir;
			vnode->type = vnodeDir;
		}

	if (!entry) {
		vnode_get(id);
		vnode->type = vnodeDir;
		vnode->path =

if (entry->type != vnodeDir)
		err = ERR_ARG;

	if (file->

	if ((realpath = malloc(strlen(sysdir) + 1 + strlen(path) + 1)) == NULL)
		*(u32 *)msg->data = 0;
	else {
		sprintf(realpath, "%s/%s", sysdir, path);

		if (flags == PHFS_RDONLY)
			ofd = open(realpath, f);
		else
			ofd = open(realpath, f, S_IRUSR | S_IWUSR);

		printf("[%d] phfs: MSG_OPEN path='%s', realpath='%s', ofd=%d\n", getpid(), path, realpath, ofd);
		*(u32 *)msg->data = ofd > 0 ? ofd : 0;
		free(realpath);
	}

	if (msg_send(fd, msg) < 0)
		return ERR_PHFS_IO;
	return 1;
}
#endif


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
		case MSG_RESET:
			res = phfs_reset(fd, msg, sysdir);
			break;
		case MSG_FSTAT:
			res = phfs_stat(fd, msg, sysdir);
			break;
	}
	if (res < 0)
		printf("[%d] phfs: msg error %d \n", getpid(), res);

	return res;
}
