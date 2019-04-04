#
# Makefile for Phoenix-RTOS phoenixd (Phoenix daemon)
#
# Copyright 2019 Phoenix Systems
#

$(PREFIX_PROG)phoenixd: $(addprefix $(PREFIX_O)phoenixd/,phoenixd.o bsp.o dispatch.o msg.o msg_udp.o phfs.o phoenixd.o) $(addprefix $(PREFIX_O)common/,serial.o usb_vybrid.o usb_imx.o)
	$(LINK)

all: $(PREFIX_PROG_STRIPPED)phoenixd
