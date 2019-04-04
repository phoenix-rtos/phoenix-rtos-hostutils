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
#include <getopt.h>

#include "../common/types.h"
#include "../common/errors.h"
#include "../common/serial.h"
#include "../phoenixd/bsp.h"
#include "../phoenixd/msg_udp.h"
#include "../phoenixd/dispatch.h"

enum {
	SDP
};

extern char *optarg;


#define VERSION "1.3"

int phoenixd_session(char *tty, char *kernel, char *sysdir)
{
	u8 t;
	int fd, count, err;
	u8 buff[BSP_MSGSZ];

	fprintf(stderr, "[%d] Starting phoenixd-child on %s\n", getpid(), tty);

	if ((fd = serial_open(tty, B460800)) < 0) {
		fprintf(stderr, "[%d] Can't open %s [%d]!\n", getpid(), tty, fd);
		return ERR_PHOENIXD_TTY;
	}

	for (;;) {
		if ((count = bsp_recv(fd, &t, (char*)buff, BSP_MSGSZ, 0)) < 0) {
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
			if ((err = bsp_sendprogram(fd, (char*)&buff[2], sysdir)) < 0)
				fprintf(stderr, "[%d] Sending program error [%d]!\n", getpid(), err);
			break;
		}
	}
	return 0;
}


int sdp_execute(void *dev)
{
	return 0;
}


int main(int argc, char *argv[])
{
	int c;
	int ind;
	int len, len2;
	char bspfl = 0;
	char *kernel = "../kernel/phoenix";

	int sdp = 0;
	int help = 0;
	int opt_idx = 0;
	char *initrd = NULL;
	char *console = NULL;
	char *append = NULL;
	char *output = NULL;
	void *dev = NULL;

	char *sysdir = "../sys";
	char *ttys[8];
	mode_t mode[8] = {SERIAL};
	int k, i = 0;
	int res, st;
	int type, proto;

	struct option long_opts[] = {
		{"sdp", no_argument, &sdp, 1},
		{"plugin", no_argument, &sdp, 2},
		{"upload", no_argument, &sdp, 3},
		{"kernel", required_argument, 0, 'k'},
		{"console", required_argument, 0, 'c'},
		{"initrd", required_argument, 0, 'I'},
		{"append", required_argument, 0, 'a'},
		{"execute", required_argument, 0, 'x'},
		{"help", no_argument, &help, 1},
		{"output", required_argument, 0, 'o'},
		{0, 0, 0, 0}};

	while (1) {
		c = getopt_long(argc, argv, "k:p:s:1m:i:u:a:x:c:I:", long_opts, &opt_idx);
		if (c < 0)
			break;

		switch (c) {
		case 'd':
			kernel = optarg;
			break;
		case 'h':
			sysdir = optarg;
			break;
		default:
			break;
		}
	}

	switch (type) {
	}

	if (proto == SDP) {
		sdp_execute(dev);
	}

	return 0;
}
