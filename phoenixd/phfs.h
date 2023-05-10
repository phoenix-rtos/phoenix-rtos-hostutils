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
 * See the LICENSE
 */

#ifndef _PHFS_H_
#define _PHFS_H_


#define MSG_OPEN   1
#define MSG_READ   2
#define MSG_WRITE  3
#define MSG_CLOSE  4
#define MSG_RESET  5
#define MSG_FSTAT   6
#define MSG_HELLO	7

/* Opening flags */
#define PHFS_RDONLY  0
#define PHFS_RDWR    1
#define PHFS_CREATE  2


typedef struct _msg_phfsio_t {
	u32 handle;
	u32 pos;
	s32 len;
	u8  buff[MSG_MAXLEN - (2u * sizeof(u32)) - sizeof(s32)];
} msg_phfsio_t;


extern int phfs_handlemsg(int fd, msg_t *msg, char *sysdir);

struct	pho_stat
{
	u32 st_dev;
	u32 st_ino;
	u16 st_mode;
	u16 st_nlink;
	u16 st_uid;
	u16 st_gid;
	u32 st_rdev;
	u32 st_size;

	u32 st_atime_;
	u32 st_mtime_;
	u32 st_ctime_;
	s32 st_blksize;
	s32 st_blocks;
};

#endif
