/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * UDP communication routines
 *
 * Copyright 2012, 2013 Phoenix Systems
 * Author: Jacek Popko
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <hostutils-common/errors.h>
#include "msg_udp.h"
#include "phfs.h"

#undef HEXDUMP

static struct sockaddr_in addr;
static socklen_t addrlen;


extern u32 msg_csum(msg_t *msg);


in_addr_t bcast_addr(in_addr_t in_addr)
{
	struct ifaddrs *ifaddr, *ifa;
	struct sockaddr_in *inet_addr;
	in_addr_t in_bcast = 0;

	if (getifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		return -1;
	}

	/* Walk through linked list, maintaining head pointer so we
	   can free list later */

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
			continue;

		inet_addr = (struct sockaddr_in *)ifa->ifa_addr;
		if (inet_addr->sin_addr.s_addr == in_addr) {

			inet_addr = (struct sockaddr_in *)ifa->ifa_netmask;
			in_bcast = in_addr | ~inet_addr->sin_addr.s_addr;
			break;
		}
	}
	freeifaddrs(ifaddr);
	return in_bcast;
}


int udp_open(char *node, uint port)
{
	int fd, result, so_enable = 1;
	struct addrinfo *servAddr;
	struct sockaddr_in addr_in, bcast_in;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return ERR_SERIAL_INIT;

	if ((result = getaddrinfo(node, NULL, NULL, &servAddr)) != 0) {
		fprintf(stderr, "Error opening %s:%d: %s\n", node, port, gai_strerror(result));
		return result;
	}

	addr_in = *(struct sockaddr_in *)servAddr->ai_addr;

	freeaddrinfo(servAddr);

	if (addr_in.sin_port == 0)
		addr_in.sin_port = htons((unsigned short)port);

	result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &so_enable, sizeof(so_enable));
	result = bind(fd, (struct sockaddr *)&addr_in, sizeof(addr_in));

	bcast_in.sin_addr.s_addr = bcast_addr(addr_in.sin_addr.s_addr);
	bcast_in.sin_port = htons(PHFS_UDPPORT);
	bcast_in.sin_family = addr_in.sin_family;

	if (result < 0)
		return ERR_SERIAL_INIT;

	if (!fork())
	{
		int bcastfd;
		ssize_t len;
		msg_t bcast_msg;
		u8 buff[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
		unsigned int i = 0;

		memset(&bcast_msg, 0, sizeof(bcast_msg));
		msg_settype(&bcast_msg, MSG_HELLO);
		msg_setlen(&bcast_msg, sizeof(bcast_in));
		memcpy(bcast_msg.data, &addr_in, sizeof(addr_in));
		bcast_msg.csum = msg_csum(&bcast_msg);

#ifdef PHFS_UDPENCODE
		{
			u8 *p = (u8 *)&bcast_msg, cs[2];
			unsigned k;

			cs[0] = MSG_MARK;

			buff[i++] = cs[0];

			for (k = 0; k < MSG_HDRSZ + msg_getlen(&bcast_msg); k++) {

				if ((p[k] == MSG_MARK) || (p[k] == MSG_ESC)) {
					cs[0] = MSG_ESC;
					if (p[k] == MSG_MARK)
						cs[1] = MSG_ESCMARK;
					else
						cs[1] = MSG_ESCESC;
					memcpy(&buff[i], cs, 2);
					i += 2;
				}
				else
					buff[i++] = p[k];
			}
		}
#else
		i = MSG_HDRSZ + msg_getlen(&bcast_msg);
		memcpy(buff, &bcast_msg, i);
#endif
		bcastfd = dup(fd);
		setsockopt(bcastfd, SOL_SOCKET, SO_BROADCAST, &so_enable, sizeof(so_enable));
		for (;;) {
			if ((len = sendto(bcastfd, buff, i, MSG_DONTROUTE, (struct sockaddr *)&bcast_in, sizeof(bcast_in))) < 0)
				return ERR_MSG_IO;
			sleep(3);
		}
	}
	return fd;
}


#ifdef HEXDUMP
static void hex_dump(void *data, int size)
{
	/* dumps size bytes of *data to stdout. Looks like:
	 * [0000] 75 6E 6B 6E 6F 77 6E 20 30 FF 00 00 00 00 39 00 unknown 0.....9.
	 */

	unsigned char *p = data;
	unsigned char c;
	int n;
	char bytestr[4] = {0};
	char addrstr[10] = {0};
	char hexstr[ 16*3 + 5] = {0};
	char charstr[16*1 + 5] = {0};
	for(n=1;n<=size;n++) {
		if (n%16 == 1) {
			/* store address for this line */
			snprintf(addrstr, sizeof(addrstr), "%.4x",
			   ((unsigned int)p-(unsigned int)data) );
		}

		c = *p;
		if (isalnum(c) == 0) {
			c = '.';
		}

		/* store hex str (for left side) */
		snprintf(bytestr, sizeof(bytestr), "%02X ", *p);
		strncat(hexstr, bytestr, sizeof(hexstr)-strlen(hexstr)-1);

		/* store char str (for right side) */
		snprintf(bytestr, sizeof(bytestr), "%c", c);
		strncat(charstr, bytestr, sizeof(charstr)-strlen(charstr)-1);

		if(n%16 == 0) {
			/* line completed */
			printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
			hexstr[0] = 0;
			charstr[0] = 0;
		} else if(n%8 == 0) {
			/* half line: add whitespaces */
			strncat(hexstr, "  ", sizeof(hexstr)-strlen(hexstr)-1);
			strncat(charstr, " ", sizeof(charstr)-strlen(charstr)-1);
		}
		p++; /* next byte */
	}

	if (strlen(hexstr) > 0) {
		/* print rest of buffer if not empty */
		printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
	}
}
#endif


int msg_udp_send(int fd, msg_t *msg, u16 seq)
{
	unsigned int k;
	u8 buff[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
	ssize_t len;
	unsigned int i = 0;

	msg_setseq(msg, seq);
	msg_setcsum(msg, msg_csum(msg));

	if (msg_getlen(msg) > MSG_MAXLEN)
		return ERR_MSG_ARG;

	i = MSG_HDRSZ + msg_getlen(msg);
	memcpy(buff, msg, i);
	k = i;

#ifdef HEXDUMP
	hex_dump(buff, i);
#endif
	if ((len = sendto(fd, buff, i, 0, (struct sockaddr *)&addr, addrlen)) < 0)
		return ERR_MSG_IO;

	if (len < i)
		return ERR_MSG_IO;

	return k;
}


int msg_udp_recv(int fd, msg_t *msg, int *state)
{
	u8 buff[2 * sizeof(msg_t)];
	ssize_t bufflen;

	addrlen = sizeof(addr);
	if ((bufflen = recvfrom(fd, buff, sizeof(buff), 0, (struct sockaddr *)&addr, &addrlen)) < 0) {
		*state = MSGRECV_DESYN;
		return ERR_MSG_IO;
	}


	memcpy(msg, buff, bufflen);
	return bufflen;
}
