/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * Error codes
 *
 * Copyright 2011 Phoenix Systems
 * Copyright 2004 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
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
#define ERR_MSG_CLOSED -50

#define ERR_SERIAL_OK       -64
#define ERR_SERIAL_INIT     -65
#define ERR_SERIAL_SETATTR  -66
#define ERR_SERIAL_IO       -67
#define ERR_SERIAL_TIMEOUT  -68
#define ERR_SERIAL_CLOSED   -69

#define ERR_PHFS_IO  -80

#define ERR_PHOENIXD_TTY    -128


#endif
