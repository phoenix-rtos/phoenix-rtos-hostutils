/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * UDP communication routines
 *
 * Copyright 2012 Phoenix Systems
 *
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
#include <netdb.h>
#include "errors.h"
#include "types.h"
#include "msg_udp.h"

#undef HEXDUMP

static struct sockaddr_in addr;
static socklen_t addrlen;


int udp_open(char *node, uint port)
{
	int fd, result;
	struct addrinfo *servAddr;
	struct sockaddr_in *addr_in;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return ERR_SERIAL_INIT;

	if ((result = getaddrinfo(node, NULL, NULL, &servAddr)) != 0) {
		fprintf(stderr, "Error opening %s:%d: %s\n", node, port, gai_strerror(result));
		return result;
	}

	addr_in = (struct sockaddr_in *)servAddr->ai_addr;
	if (addr_in->sin_port == 0)
		addr_in->sin_port = htons((unsigned short)port);

	result = bind (fd, servAddr->ai_addr, servAddr->ai_addrlen);
	freeaddrinfo(servAddr);

	if (result < 0)
		return ERR_SERIAL_INIT;

	return fd;
}


static u32 msg_csum(msg_t *msg)
{
	unsigned int k;
	u32 csum;
	
	csum = 0;
	for (k = 0; k < MSG_HDRSZ + msg_getlen(msg); k++) {
		if (k >= sizeof(msg->csum))
			csum += *((u8 *)msg + k);
	}
	return csum;
}

#ifdef HEXDUMP
static void hex_dump(void *data, int size)
{
    /* dumps size bytes of *data to stdout. Looks like:
     * [0000] 75 6E 6B 6E 6F 77 6E 20
     *                  30 FF 00 00 00 00 39 00 unknown 0.....9.
     * (in a single line of course)
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

int msg_udp_send(int fd, msg_t *msg)
{
	u8 *p = (u8 *)msg;
	u8 cs[2];
	unsigned int k;
	u8 buff[MSG_MAXLEN * 2 + MSG_HDRSZ * 2];
	ssize_t len;
	unsigned int i = 0;
	
	msg->csum = msg_csum(msg);
	cs[0] = MSG_MARK;
	
	if (msg_getlen(msg) > MSG_MAXLEN)
		return ERR_MSG_ARG;

	buff[i++] = cs[0];
	
	for (k = 0; k < MSG_HDRSZ + msg_getlen(msg); k++) {
	
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
	int escfl = 0;
	u8 buff[2 * sizeof(msg_t)], *buffptr;
	unsigned int l = 0;
	ssize_t bufflen;

	addrlen = sizeof(addr);
	if ((bufflen = recvfrom(fd, buff, sizeof(buff), 0, (struct sockaddr *)&addr, &addrlen)) < 0) {
		*state = MSGRECV_DESYN;
		return ERR_MSG_IO;
	}

	for (buffptr = buff; buffptr < buff + bufflen; buffptr++) {
			
		if (*state == MSGRECV_FRAME) {
			
			/* Return error if frame is to long */
			if (l == MSG_HDRSZ + MSG_MAXLEN) {
				*state = MSGRECV_DESYN;
				return ERR_MSG_IO;
			}
				
			/* Return error if terminator discovered */
			if (*buffptr == MSG_MARK) {
				return ERR_MSG_IO;
			}
			
			if (!escfl && (*buffptr == MSG_ESC)) {
				escfl = 1;
				continue;
			}
			if (escfl) {
				if (*buffptr == MSG_ESCMARK)
					*buffptr = MSG_MARK;
				if (*buffptr == MSG_ESCESC)
					*buffptr = MSG_ESC;
				escfl = 0;
			}
			*((u8 *)msg + l++) = *buffptr;
			
			/* Frame received */ 
			if ((l >= MSG_HDRSZ) && (l == msg_getlen(msg) + MSG_HDRSZ)) {
				*state = MSGRECV_DESYN;
				break;
			}
		}
		else {
			/* Synchronize */
			if (*buffptr == MSG_MARK)
				*state = MSGRECV_FRAME;
		}
	}
	
	/* Verify received message */
	if (msg->csum != msg_csum(msg)) {
		return ERR_MSG_IO;
	}

	return l;
}
