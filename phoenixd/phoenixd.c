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

#include <hostutils-common/types.h>
#include <hostutils-common/errors.h>
#include <hostutils-common/serial.h>
#include <hostutils-common/dispatch.h>
#include "bsp.h"
#include "msg_udp.h"
#include "msg_tcp.h"
#include "dispatch.h"


extern char *optarg;

#define VERSION "1.5"


int phoenixd_session(char *tty, char *kernel, char *sysdir, speed_t baudrate)
{
	u8 t;
	int fd, count, err;
	u8 buff[BSP_MSGSZ];

	fprintf(stderr, "[%d] Starting phoenixd-child on %s\n", getpid(), tty);

	if ((fd = serial_open(tty, baudrate)) < 0) {
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


void print_help(void)
{
	fprintf(stderr, "usage: phoenixd [-1] [-k kernel] [-s bindir]\n"
			"\t\t-p serial_device [ [-p serial_device] ... ]\n"
			"\t\t-m pipe_file [ [-m pipe_file] ... ]\n"
			"\t\t-i udp_ip_addr:port [ [-i udp_ip_addr:port] ... ]\n"
			"\t\t-t tcp_ip_addr:port [ [-t tcp_ip_addr:port] ... ]\n"
			"\t\t-u load_addr[:jump_addr]\n");

	fprintf(stderr, "\n"
		"For imx6ull:\n"
		"\n"
		"Modes:\n"
		"--sdp\t\t- Make image for older kernels version without plugin. Image\n"
		"\t\t  will contain only kernel + initrd and it is limited to 68KB.\n"
		"\t\t  It is expected initrd will download the rest of the modules\n"
		"\t\t  (console + append).\n"
		"--plugin\t- Make image with all modules in syspage for kernels with\n"
		"\t\t  plugin. Image size is limited to 4MB. In this mode arguments\n"
		"\t\t  are passed only to kernel e.g.\n"
		"\t\t  <kernel_path>=\"app1;arg1;arg2 app2;arg1;arg2\".\n"
		"--upload\t- Just like the sdp mode but for kernels with plugin. Image\n"
		"\t\t  size is limited to 4MB.\n"
		"\n"
		"Arguments:\n"
		"-k, --kernel\t- kernel image path\n"
		"-c, --console\t- console server path\n"
		"-I, --initrd\t- initrd server path\n"
		"-x, --execute\t- path to servers appended to initrd with optional arguments\n"
		"\t\t  (they will be automatically executed),\n"
		"-a, --append\t- path to servers appended to initrd with optional arguments,\n"
		"\t\t  prefix path with F to fetch or X to fetch and execute (only\n"
		"\t\t  in sdp and upload modes) example:\n"
		"\t\t  --append Xpath1=arg1,arg2 Fpath2=arg1,arg2\n"
		"-o, --output\t- output file path. By default image is uploaded.\n"
		"-h, --help\t- prints this message\n");
}


int main(int argc, char *argv[])
{
	int c;
	int ind;
	int len = 0, len2;
	char bspfl = 0;
	char *kernel = "../kernel/phoenix";

	int sdp = 0;
	int opt_idx = 0;
	char *initrd = NULL;
	char *console = NULL;
	char *append = NULL;
	char *output = NULL;

	speed_t speed;
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
		{"execute", required_argument, 0, 'x'},
		{"help", no_argument, 0, 'h'},
		{"baudrate", required_argument, 0, 'b'},
		{"output", required_argument, 0, 'o'},
		{0, 0, 0, 0}};

	printf("-\\- Phoenix server, ver. " VERSION "\n"
		"(c) 2012 Phoenix Systems\n"
		"(c) 2000, 2005 Pawel Pisarczyk\n"
		"\n");

	if (serial_int2speed(460800, &speed) < 0) {
		fprintf(stderr, "Wrong baudrate's value!\n");
		return ERR_ARG;
	}

	while (1) {
		c = getopt_long(argc, argv, "h1k:p:s:m:i:u:a:x:c:I:o:b:t:", long_opts, &opt_idx);
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
		case 'b' :
			if (serial_int2speed(atoi(optarg), &speed) < 0) {
				fprintf(stderr, "Wrong baudrate's value!\n");
				return ERR_ARG;
			}
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
		case 't':
			if (i >= 8) {
				fprintf(stderr, "Too many instances (-t %s)\n", optarg);
				break;
			}
			mode[i] = TCP;
			ttys[i++] = optarg;
			break;
		case 'u':
			if (i >= 8) {
				fprintf(stderr, "Too many instances (-u %s)\n", optarg);
				break;
			}
			mode[i] = USB_VYBRID;
			ttys[i++] = optarg; /* Load address */
			break;
		case 'a':
		case 'x':
			ind = optind - 1;
			len2 = 0;
			while (ind < argc && *argv[ind] != '-') {
				len2 += strlen(argv[ind]) + 1 + (c == 'x');
				ind++;
			}
			append = realloc(append, len + len2 + 1);
			ind = optind - 1;
			while (ind < argc && *argv[ind] != '-') {
				len += sprintf(append + len, c == 'x' ? "X%s " : "%s " , argv[ind]);
				ind++;
			}
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
		case 'h':
		case '?':
			print_help();
			return -1;
		default:
			break;
		}
	}

	if (output) {
		if (kernel == NULL) {
			fprintf(stderr, "Output file needs kernel path\n");
			return -1;
		}
		res = boot_image(kernel, initrd, console, append, output, sdp == 2 ? 1 : 0);
		return res;
	}

	if (sdp) {
		res = 0;
		if (sdp == 1)
			res = usb_imx_dispatch(kernel, console, initrd, append, 0);
		else if (sdp == 2)
			res = boot_image(kernel, initrd, console, append, NULL, 1);
		else if (sdp == 3)
			res = usb_imx_dispatch(kernel, console, initrd, append, 1);
		return res;
	}

	if (i == 0) {
		fprintf(stderr, "You have to specify at least one serial device, pipe or IP address\n\n");
		print_help();
		return -1;
	}

	free(append);
	for (k = 0; k < i; k++) {
		res = fork();
		if(res < 0) {
			fprintf(stderr, "Fork error for %d child!\n", k);
			continue;
		} else if(res == 0) {
			if (bspfl)
				res = phoenixd_session(ttys[k], kernel, sysdir, speed);
			else if(mode[k] == USB_VYBRID) {
				char *jumAddr = NULL;
				if ((jumAddr = strchr(ttys[k], ':')) != NULL)
					*jumAddr++ = '\0';

				res = usb_vybrid_dispatch(kernel,ttys[k], jumAddr, NULL, 0);
			} else {
				char *port;
				unsigned speed_port = 0;

				if (mode[k] == UDP) {
					port = strchr(ttys[k], ':');
					if (port != NULL) {
						*port++ = '\0';
						sscanf(port, "%u", &speed_port);
					}

					if ((speed_port == 0) || (speed_port > 0xffff)) {
						speed_port = PHFS_UDPPORT;
					}

					res = dispatch(ttys[k], mode[k], sysdir, (void *)&speed_port);
				}
				else if (mode[k] == TCP) {
					port = strchr(ttys[k], ':');
					if (port != NULL) {
						*port++ = '\0';
						sscanf(port, "%u", &speed_port);
					}

					if ((speed_port == 0) || (speed_port > 0xffff)) {
						speed_port = PHFS_TCPPORT;
					}

					res = dispatch(ttys[k], mode[k], sysdir, (void *)&speed_port);
				}
				else {
					res = dispatch(ttys[k], mode[k], sysdir, (void *)&speed);
				}
			}
			return res;
		} //else
			//free(ttys[k]);
	}

	for (k = 0; k < i; k++)
		wait(&st);
	return 0;
}
