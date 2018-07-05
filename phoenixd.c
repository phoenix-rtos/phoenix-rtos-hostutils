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


int main(int argc, char *argv[])
{
	int c;
	int ind;
	int len;
	char bspfl = 0;
	char *kernel = "../kernel/phoenix";

	int sdp = 0;
	int help = 0;
	int opt_idx = 0;
	char *initrd = NULL;
	char *console = NULL;
	char *append = NULL;
	char *output = NULL;

	char *sysdir = "../sys";
	char *ttys[8];
	mode_t mode[8] = {SERIAL};
	int k, i = 0;
	int res, st;

	struct option long_opts[] = {
		{"sdp", no_argument, &sdp, 1},
		{"plugin", no_argument, &sdp, 2},
		{"upload", no_argument, &sdp, 3},
		{"kernel", required_argument, 0, 'k'},
		{"console", required_argument, 0, 'c'},
		{"initrd", required_argument, 0, 'I'},
		{"append", required_argument, 0, 'a'},
		{"help", no_argument, &help, 1},
		{"output", required_argument, 0, 'o'},
		{0, 0, 0, 0}};

	printf("-\\- Phoenix server, ver. " VERSION "\n(c) 2000, 2005 Pawel Pisarczyk\n(c) 2012 Phoenix Systems\n");

	while (1) {
		c = getopt_long(argc, argv, "k:p:s:1m:i:u:a:c:I:", long_opts, &opt_idx);
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
		case 'u':
			mode[i] = USB_VYBRID;
			ttys[i++] = optarg; /* Load address */
			break;
		case 'a':
			ind = optind - 1;
			len = 0;
			while (ind < argc && *argv[ind] != '-') {
				len += strlen(argv[ind]) + 1;
				ind++;
			}
			append = malloc(len + 1);
			ind = optind - 1;
			len = 0;
			while (ind < argc && *argv[ind] != '-') {
				sprintf(append + len, "%s ", argv[ind]);
				len += strlen(argv[ind]) + 1;
				ind++;
			}
			append[len - 1] = '\0';
			break;
		case 'I':
			initrd = optarg;
			break;
		case 'c':
			console = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		default:
			break;
		}
	}

	if (output) {
		if (!kernel || !initrd) {
			printf("Output file needs kernel and initrd paths\n");
			return 0;
		}
		res = boot_image(kernel, initrd, console, append, output, sdp == 2 ? 1 : 0);
		return 0;
	}

	if (sdp) {
		if (sdp == 1)
			res = usb_imx_dispatch(kernel, console, initrd, append, 0);
		else if (sdp == 2)
			res = boot_image(kernel, initrd, console, append, NULL, 1);
		else if (sdp == 3)
			res = usb_imx_dispatch(kernel, console, initrd, append, 1);
		free(append);
		return 0;
	}

	if ((!i && !sdp) || help) {
		fprintf(stderr, "You have to specify at least one serial device, pipe or IP address\n");
		fprintf(stderr, "usage: phoenixd [-1] [-k kernel] [-s bindir] "
				"-p serial_device [ [-p serial_device] ... ] -m pipe_file [ [-m pipe_file] ... ]"
				"-i ip_addr:port [ [-i ip_addr:port] ... ]"
				" -u [load_addr[:jump_addr]]\n");

		printf("\nFor imx6ull:\n\n"
				"Modes:\n"
				"--sdp\t\t- Make image for older kernels version without plugin.\n"
				"\t\t  Image will contain only kernel + initrd and it is limited to 68KB.\n"
				"\t\t  It is expected initrd will download the rest of the modules (console + append).\n"
				"--plugin\t- Make image with all modules in syspage for kernels with plugin. Image size is limited to 4MB.\n"
				"\t\t  In this mode arguments are passed only to kernel e.g. <kernel_path>=\"app1;arg1;arg2 app2;arg1;arg2\".\n"
				"--upload\t- Just like the sdp mode but for kernels with plugin. Image size is limited to 4MB.\n"
				"\nArguments:\n"
				"--kernel, -k\t- kernel image path\n"
				"--console, -c\t- console server path\n"
				"--initrd, -I\t- initrd server path\n"
				"--append, -a\t- path to servers appended to initrd with optional arguments,\n"
				"\t\t  prefix path with F to fetch or X to fetch and execute (only in sdp and upload modes)\n"
				"\t\t  example: --append Xpath1=arg1,arg2 Fpath2=arg1,arg2\n"
				"--output, -o\t- output file path. By default image is uploaded.\n"
				"--help, -h\t- prints this message\n");
		return -1;
	}

	for (k = 0; k < i; k++) {
		res = fork();
		if(res < 0) {
			fprintf(stderr,"Fork error for %d child!\n",k);
			continue;
		} else if(res == 0) {
			if (bspfl)
				res = phoenixd_session(ttys[k], kernel, sysdir);
			else if(mode[k] == USB_VYBRID) {
				char *jumAddr = NULL;
				if ((jumAddr = strchr(ttys[k], ':')) != NULL)
					*jumAddr++ = '\0';

				res = usb_vybrid_dispatch(kernel,ttys[k], jumAddr, NULL, 0);
			} else {
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
					speed_port = B460800;

				res = dispatch(ttys[k], mode[k], speed_port, sysdir);
			}
			return res;
		} //else
			//free(ttys[k]);
	}

	for (k = 0; k < i; k++)
		wait(&st);
	return 0;
}
