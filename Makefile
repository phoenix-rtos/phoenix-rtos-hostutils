#
# phoenixd -  Phoenix server
# (c) Pawel Pisarczyk, 2001
#

CC = gcc
LD = gcc
CFLAGS = -c -Wall -I . -O2
LDFLAGS = -lm

SRCS = serial.c bsp.c dispatch.c msg.c phfs.c phoenixd.c
OBJS = $(SRCS:.c=.o)
BIN = phoenixd

all: phoenixd

.c.o:
	$(CC) $(CFLAGS) $<

$(OBJS): serial.h bsp.h errors.h elf.h

phoenixd: $(OBJS)
	$(LD) $(LDFLAGS) -o $(BIN) $(OBJS)

clean:
	rm -f *.o *~ core
