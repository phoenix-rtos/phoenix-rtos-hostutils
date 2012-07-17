/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * BSP2 message dispatcher
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "errors.h"
#include "serial.h"
#include "msg.h"
#include "dispatch.h"
#include "phfs.h"


static char *concat(char *s1, char *s2)
{
	char *result = malloc(strlen(s1) + strlen(s2) + 1);
	strcpy(result, s1);
	strcat(result, s2);
	return result;

}

/* Function reads and dispatches messages */
int dispatch(char *dev, int is_pipe, unsigned int speed, char *sysdir)
{
	int fd = -1;
	int fd_out = -1;
	msg_t msg;
	int state, err;
	char *dev_in = 0; 
	char *dev_out = 0;
	
	printf("[%d] dispatch: Starting message dispatcher on %s\n", getpid(), dev);
	if (!is_pipe){
		if ((fd = serial_open(dev, speed)) < 0) {
			fprintf(stderr, "[%d] dispatch: Can't open serial port '%s'\n", getpid(), dev);		
			return ERR_DISPATCH_IO;
		}
	} else {
		dev_in = concat(dev, ".out"); // because output from quemu is our input
		dev_out = concat(dev, ".in"); // same logic

		if ((fd = open(dev_in, O_RDONLY)) < 0) {
			fprintf(stderr, "[%d] dispatch: Can't open pipe '%s'\n", getpid(), dev_in);	
			free(dev_in);
			free(dev_out);
			return ERR_DISPATCH_IO;
		}

		if ((fd_out = open(dev_out, O_WRONLY)) < 0) {
			fprintf(stderr, "[%d] dispatch: Can't open pipe '%s'\n", getpid(), dev_out);		
			free(dev_in);
			free(dev_out);
			return ERR_DISPATCH_IO;
		}

		free(dev_in);
		free(dev_out);
	}

	for (state = MSGRECV_DESYN;;) {
		if (msg_recv(fd, &msg, &state) < 0) {
			fprintf(stderr, "[%d] dispatch: Message receiving error on %s, state=%d!\n", getpid(), dev, state);
			continue;
		}
		
		if (err = phfs_handlemsg((is_pipe ? fd_out : fd), &msg, sysdir))
			continue;

		switch (msg_gettype(&msg)) {
		case MSG_ERR:
			msg_settype(&msg, MSG_ERR);
			msg_setlen(&msg, MSG_MAXLEN);
			msg_send((is_pipe ? fd_out : fd), &msg);
			break;
		}
			
	}
	return 0;
}
