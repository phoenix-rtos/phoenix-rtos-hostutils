#
# Makefile for Phoenix-RTOS psdisk (Phoenix Systems Disk Tool)
#
# Copyright 2020 Phoenix Systems
#

# TODO: fix per project board_config.h visibility
CFLAGS += -I$(PROJECT_PATH)/

$(PREFIX_PROG)psdisk: $(PREFIX_O)psdisk/psdisk.o $(PREFIX_O)../phoenix-rtos-corelibs/libptable/ptable.o
	$(LINK)

all: $(PREFIX_PROG_STRIPPED)psdisk
