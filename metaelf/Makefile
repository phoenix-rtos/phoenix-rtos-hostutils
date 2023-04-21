#
# Makefile for Phoenix-RTOS syspagen
#
# Copyright 2022 Phoenix Systems
#

ifneq ($(shell uname), Darwin)

$(PREFIX_PROG)metaelf: $(addprefix $(PREFIX_O)metaelf/,crc32.o metaelf.o)
	$(LINK)

all: $(PREFIX_PROG_STRIPPED)metaelf

endif
