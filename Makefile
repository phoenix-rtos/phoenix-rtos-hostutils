#
# phoenixd -  Phoenix server
#
# Copyright 2018 Phoenix Systems
# Copyright 2001 Pawel Pisarczyk
#

SIL = @
CC = gcc
CFLAGS = -c -Wall -I . -O2 -g
LD = gcc
LDFLAGS = -lm -lusb-1.0

OBJS = serial.o bsp.o dispatch.o msg.o msg_udp.o phfs.o phoenixd.o usb_vybrid.o usb_imx.o

all: phoenixd

.c.o:
	@echo "CC" $<
	$(SIL)$(CC) $(CFLAGS) $<

$(OBJS): serial.h bsp.h errors.h elf.h

phoenixd: $(OBJS)
	@echo "LINK" $@
	$(SIL)$(LD) -o $@ $(OBJS) $(LDFLAGS)

.PHONY: clean
clean:
	@echo "CLEAN"

ifneq ($(filter clean,$(MAKECMDGOALS)),)
	$(shell rm -rf *.o core)
endif
