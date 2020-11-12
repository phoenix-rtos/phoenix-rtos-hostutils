/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * Serial interface accessing routines
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2001 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * See the LICENSE
 */

#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <sys/types.h>
#include <fcntl.h>
#include <termios.h>
#include "types.h"


extern int serial_open(char *dev, speed_t speed);


extern int serial_read(int fd, u8 *buff, uint len, uint timeout);


extern int serial_write(int fd, u8 *buff, uint len);


extern int serial_int2speed(int baudrate, speed_t *speed);


extern int serial_speed2int(speed_t speed, int *baudrate);


#endif
