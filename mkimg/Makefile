#
# mkimg-imx - generates image for i.MX6ULL
# (c) Pawel Pisarczyk, 2001
#

CC = gcc
LD = gcc
CFLAGS = -c -Wall -I . -O0 -g

SRCS = mkimg-imx.c
OBJS = $(SRCS:.c=.o)
BIN = mkimg-imx

all: mkimg-imx

.c.o:
	$(CC) $(CFLAGS) $<

phoenixd: $(OBJS)
	$(LD) $(LDFLAGS) -o $(BIN) $(OBJS)

clean:
	rm -f *.o *~ $(BIN)
