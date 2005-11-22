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
#include <unistd.h>

#include "errors.h"
#include "serial.h"
#include "msg.h"
#include "dispatch.h"
#include "phfs.h"


/* Function reads and dispatches messages */
int dispatch(char *dev, unsigned int speed, char *sysdir)
{
	int fd;
	msg_t msg;
	int state;
	
	printf("[%d] dispatch: Starting message dispatcher on %s\n", getpid(), dev);
			
	if ((fd = serial_open(dev, speed)) < 0) {
		fprintf(stderr, "[%d] dispatch: Can't open serial port '%s'\n", getpid(), dev);		
		return ERR_DISPATCH_IO;
	}

	for (state = MSGRECV_DESYN;;) {
		if (msg_recv(fd, &msg, &state) < 0) {
			fprintf(stderr, "[%d] dispatch: Message receiving error on %s, state=%d!\n", getpid(), dev, state);
			continue;
		}
		
		if (phfs_handlemsg(fd, &msg, sysdir))
			continue;
		
		switch (msg_gettype(&msg)) {
		case MSG_ERR:
			msg_settype(&msg, MSG_ERR);
			msg_setlen(&msg, MSG_MAXLEN);
			msg_send(fd, &msg);
			break;
		}
			
	}
	return 0;
}
