/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * Copyright 2001, 2004 Pawel Pisarczyk
 * Copyright 2012 Phoenix Systems
 *
 * Author: Pawel Pisarczyk, Jacek Popko
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <stdlib.h>

#include "types.h"
#include "errors.h"
#include "serial.h"
#include "bsp.h"
#include "msg_udp.h"
#include "dispatch.h"


extern char *optarg;


#define VERSION "1.3"

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
	mode_t mode[8] = {SERIAL};
	int k, i = 0;
	int res, st;
	
	printf("-\\- Phoenix server, ver. " VERSION "\n(c) 2000, 2005 Pawel Pisarczyk\n(c) 2012 Phoenix Systems\n");
	
	while (1) {	
		c = getopt(argc, argv, "k:p:s:1m:i:");
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
				mode[i] = PIPE;
			}
		case 'p':
			if (i == 8) {
				fprintf(stderr, "Too many ttys for open!\n");
				return ERR_ARG;
			}

			ttys[i++] = optarg;
			break;

		case 'i':
			if (i >= 8) {
				fprintf(stderr, "Too many instances (-i %s)\n", optarg);
				break;
			}
			mode[i] = UDP;
			ttys[i++] = optarg;
			break;

		default:
			break;
		}
	}
		
	if (!i) {
		fprintf(stderr, "You have to specify at least one serial device, pipe or IP address\n");
		fprintf(stderr, "usage: phoenixd [-1] [-k kernel] [-s bindir] "
				"-p serial_device [ [-p serial_device] ... ] -m pipe_file [ [-m pipe_file] ... ]"
				"-i ip_addr:port [ [-i ip_addr:port] ... ]\n");
		return -1;
	}
	for (k = 0; k < i; k++)
		if (!fork()) {
		
			if (bspfl)
				res = phoenixd_session(ttys[k], kernel, sysdir);
			else {
				unsigned speed_port = 0;

				if (mode[k] == UDP)	{
					char *port;

					if ((port = strchr(ttys[k], ':')) != NULL)
					{
						*port++ = '\0';
						sscanf(port, "%u", &speed_port);
					}

					if (speed_port == 0 || speed_port > 0xffff)
						speed_port = PHFS_DEFPORT;
				}
				else
					speed_port = B115200;

				res = dispatch(ttys[k], mode[k], speed_port, sysdir);
			}
			return res;
		}	

	for (k = 0; k < i; k++)
		wait(&st);
	return 0;
}
