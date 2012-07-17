/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Copyright 2001, 2004 Pawel Pisarczyk
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
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

#include "types.h"
#include "errors.h"
#include "serial.h"
#include "bsp.h"
#include "dispatch.h"


extern char *optarg;


#define VERSION "1.2"


int phoenixd_session(char *tty, char *kernel, char *sysdir)
{
	u8 t;
	int fd, count, err;
	u8 buff[BSP_MSGSZ];
	
	fprintf(stderr, "[%d] Starting phoenixd-child on %s\n", getpid(), tty);
	
	if ((fd = serial_open(tty, B115200)) < 0) {
		fprintf(stderr, "[%d] Can't open %s [%d]!\n", getpid(), tty, fd);
		return ERR_PHOENIXD_TTY;
	}

	for (;;) {
		if ((count = bsp_recv(fd, &t, buff, BSP_MSGSZ, 0)) < 0) {
			bsp_send(fd, BSP_TYPE_RETR, NULL, 0);
			continue;
		}

		switch (t) {
		
		/* Handle kernel request */
		case BSP_TYPE_KDATA:
			if (*(u8 *)buff != 0) {
				fprintf(stderr, "[%d] Bad kernel request on %s\n", getpid(), tty);
				break;
			}
			fprintf(stderr, "[%d] Sending kernel to %s\n", getpid(), tty);
						
			if ((err = bsp_sendkernel(fd, kernel)) < 0) {
				fprintf(stderr, "[%d] Sending kernel error [%d]!\n", getpid(), err);
				break;
			}
			break;
		
		/* Handle program request */
		case BSP_TYPE_PDATA:	
			fprintf(stderr, "[%d] Load program request on %s, program=%s\n", getpid(), tty, &buff[2]);
			if ((err = bsp_sendprogram(fd, &buff[2], sysdir)) < 0)
				fprintf(stderr, "[%d] Sending program error [%d]!\n", getpid(), err);
			break;
		}
	}
	return 0;
}


int main(int argc, char *argv[])
{
	char c, bspfl = 0;
	char *kernel = "../kernel/phoenix";
	char *sysdir = "../sys";
	char *ttys[8];
	int is_pipe[8] = {0};
	int k, i = 0;
	int res, st;
	
	printf("-\\- Phoenix server, ver. " VERSION ", (c) Pawel Pisarczyk, 2000, 2005\n");
	
	while (1) {	
		c = getopt(argc, argv, "k:p:s:1m:1");
		if (c < 0)
			break; 				
		
		switch (c) {
		case 'k':
			kernel = optarg;
			break;
		case 's':
			sysdir = optarg;
			break;
		case '1':
			bspfl = 1;
			break;
	
		case 'm':
			if (i < 8) {
				is_pipe[i] = 1;
			}
		case 'p':
			if (i == 8) {
				fprintf(stderr, "To many ttys for open!\n");
				return ERR_ARG;
			}

			if ((ttys[i] = (char *)malloc(strlen(optarg) + 1)) == NULL)
				return ERR_MEM;
			strcpy(ttys[i++], optarg);
			break;

		default:
			break;
		}
	}
		
	if (!i) {
		fprintf(stderr, "You have to specify at least one serial devcie\n");
		fprintf(stderr, "usage: phoenixd [-1] [-k kernel] [-s bindir] -p serial_device [ [-p serial_device] ... ] -m pipe_file [ [-m pipe_file] ... ]\n");
		return -1;
	}
		
	for (k = 0; k < i; k++)
		if (!fork()) {
		
			if (bspfl)
				res = phoenixd_session(ttys[k], kernel, sysdir);
			else
				res = dispatch(ttys[k], is_pipe[k], B115200, sysdir);
			
			return res;
		}	

	for (k = 0; k < i; k++)
		wait(&st);
	return 0;
}
