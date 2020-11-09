#
# Makefile for Phoenix-RTOS psu (Phoenix Serial Uploader)
#
# Copyright 2019 Phoenix Systems
#

$(PREFIX_PROG)psu: $(PREFIX_O)psu/psu.o $(addprefix $(PREFIX_O)common/,script.o serial.o hid.o) $(PREFIX_O)phoenixd/bsp.o
	$(LINK)

all: $(PREFIX_PROG_STRIPPED)psu
