/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Serial interface accessing routines
 *
 * Copyright 2012 Phoenix Systems
 * Copyright 2001 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * See the LICENSE
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include "types.h"
#include "errors.h"
#include "serial.h"


int serial_open(char *dev, uint speed)
{
	int fd;
	struct termios newtio;

	if ((fd = open(dev, O_RDWR |  O_NONBLOCK | O_EXCL)) < 0)
		return ERR_SERIAL_INIT;

	memset(&newtio, 0, sizeof(newtio));
	cfmakeraw(&newtio);
	
	newtio.c_cflag |= CS8 | CREAD | CLOCAL;
	newtio.c_cc[VMIN] = 0;
	newtio.c_cc[VTIME] = 1;

	cfsetispeed(&newtio, speed);
	cfsetospeed(&newtio, speed);
	
	if (tcflush(fd, TCIOFLUSH) < 0)
		return ERR_SERIAL_IO;

	if (tcsetattr(fd, TCSAFLUSH, &newtio) < 0)
		return ERR_SERIAL_SETATTR;

	return fd;
}


int serial_read(int fd, u8 *buff, uint len, uint timeout)
{
	char c;
	int p = 0;
	fd_set fds;
	struct timeval tv;
	struct timeval *tvp = NULL;
	int res;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	for (;;) {
		tv.tv_sec = 0;
		tv.tv_usec = timeout * 1000;

		if (timeout)
			tvp = &tv;

		if ((res = select(fd + 1, &fds, NULL, NULL, tvp)) < 0)
			return ERR_SERIAL_IO;
		if (!res)
			return ERR_SERIAL_TIMEOUT;

		if ((res = read(fd, &c, 1)) < 0)
			return ERR_SERIAL_IO;
		
		/* if select returned readiness but we got read size zero - we got closed conn */
		else if (res == 0)
			return ERR_SERIAL_CLOSED;

		buff[p++] = c;
		if (p == len)
			break;
	}
	return p;
}


int serial_write(int fd, u8 *buff, uint len)
{
	int l;

	for (;;) {
		if ((l = write(fd, buff, len)) < 0)
			return ERR_SERIAL_IO;
		
		buff += l;
		len -= l;
		if (!len)
			break;
	}

	return len;
}
