#
# phoenixd -  Phoenix server
# (c) Pawel Pisarczyk, 2001
#

CC = gcc
LD = gcc
CFLAGS = -c -Wall -I . -O0 -g
LIBS = -lm -lusb-1.0

SRCS = serial.c bsp.c dispatch.c msg.c msg_udp.c phfs.c phoenixd.c usb_vybrid.c usb_imx.c
OBJS = $(SRCS:.c=.o)
BIN = phoenixd

all: phoenixd

.c.o:
	$(CC) $(CFLAGS) $<

$(OBJS): serial.h bsp.h errors.h elf.h

phoenixd: $(OBJS)
	$(LD) $(LDFLAGS) -o $(BIN) $(OBJS) $(LIBS)

clean:
	rm -f *.o *~ core
