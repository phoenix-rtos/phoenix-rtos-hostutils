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

#include "hostutils-common/types.h"
#include "hostutils-common/errors.h"
#include "hostutils-common/serial.h"


int serial_open(char *dev, speed_t speed)
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


int serial_int2speed(int baudrate, speed_t *speed)
{
	switch (baudrate) {
		case 0:         *speed = B0;      return 0;
		case 300:       *speed = B300;    return 0;
		case 600:       *speed = B600;    return 0;
		case 1200:      *speed = B1200;   return 0;
		case 1800:      *speed = B1800;   return 0;
		case 2400:      *speed = B2400;   return 0;
		case 4800:      *speed = B4800;   return 0;
		case 9600:      *speed = B9600;   return 0;
		case 19200:     *speed = B19200;  return 0;
		case 38400:     *speed = B38400;  return 0;
		case 57600:     *speed = B57600;  return 0;
		case 115200:    *speed = B115200; return 0;
#ifdef __APPLE__
		case 230400:    *speed = B115200; return 0;
		case 460800:    *speed = B115200; return 0;
#else
		case 230400:    *speed = B230400; return 0;
		case 460800:    *speed = B460800; return 0;
#endif
	}

	return -1;
}


int serial_speed2int(speed_t speed, int *baudrate)
{
	switch (speed) {
		case B0:        *baudrate = 0;       return 0;
		case B300:      *baudrate = 300;     return 0;
		case B600:      *baudrate = 600;     return 0;
		case B1200:     *baudrate = 1200;    return 0;
		case B1800:     *baudrate = 1800;    return 0;
		case B2400:     *baudrate = 2400;    return 0;
		case B4800:     *baudrate = 4800;    return 0;
		case B9600:     *baudrate = 9600;    return 0;
		case B19200:    *baudrate = 19200;   return 0;
		case B38400:    *baudrate = 38400;   return 0;
		case B57600:    *baudrate = 57600;   return 0;
		case B115200:   *baudrate = 115200;  return 0;
#ifdef __APPLE__
		case B230400:   *baudrate = 115200;  return 0;
#else
		case B230400:   *baudrate = 230400;  return 0;
		case B460800:   *baudrate = 460800;  return 0;
#endif
	}

	return -1;
}
