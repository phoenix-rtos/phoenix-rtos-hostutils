/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Error codes
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

#ifndef _ERRORS_H_
#define _ERRORS_H_


#define ERR_NONE   0
#define ERR_ARG   -1
#define ERR_MEM   -2
#define ERR_SIZE  -3
#define ERR_FILE  -4

#define ERR_DISPATCH_IO  -16

#define ERR_BSP_FCS   -32
#define ERR_BSP_RETR  -33

#define ERR_MSG_IO     -48
#define ERR_MSG_ARG    -49

#define ERR_SERIAL_OK       -64
#define ERR_SERIAL_INIT     -65
#define ERR_SERIAL_SETATTR  -66
#define ERR_SERIAL_IO       -67
#define ERR_SERIAL_TIMEOUT  -68

#define ERR_PHFS_IO  -80

#define ERR_PHOENIXD_TTY    -128


#endif
