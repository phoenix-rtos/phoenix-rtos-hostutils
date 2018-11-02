/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Serial interface accessing routines
 *
 * Copyright 2001 Pawel Pisarczyk
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
#include <sys/time.h>

#include "types.h"
#include "errors.h"
#include "serial.h"


int serial_open(char *dev, uint speed)
{
	int fd;
	struct termios newtio;
	
	if ((fd = open(dev, O_RDWR | O_EXCL | O_SYNC)) < 0)
		return ERR_SERIAL_INIT;

	memset(&newtio, 0, sizeof(newtio));
	cfmakeraw(&newtio);
	
	newtio.c_cflag |= CS8 | CREAD | CLOCAL;
	newtio.c_cc[VMIN] = 1;
	newtio.c_cc[VTIME] = 0;
	
	cfsetispeed(&newtio, speed);
	cfsetospeed(&newtio, speed);
	
	if (tcflush(fd, TCIFLUSH) < 0)
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

		if ((res = read(fd, &c, 1)) < 0) {
			return ERR_SERIAL_IO;
		} else if (res == 0) { // if select returned readiness but we got read size zero - we got closed conn
			return ERR_SERIAL_CLOSED;
		}

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
		if ((l = write(fd, buff, len)) < 0) {
			return ERR_SERIAL_IO;
		}
		
		buff += l;
		len -= l;
		if (!len)
			break;
	}

	return len;
}
