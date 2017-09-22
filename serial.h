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

#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <sys/types.h>
#include "types.h"


extern int serial_open(char *dev, uint speed);


extern int serial_read(int fd, u8 *buff, uint len, uint timeout);


extern int serial_write(int fd, u8 *buff, uint len);


#endif
