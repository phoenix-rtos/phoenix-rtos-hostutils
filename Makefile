#
# Makefile for phoenix-rtos-hostutils
#
# Copyright 2018-2021 Phoenix Systems
#
# %LICENSE%
#

# NOTE: currently host utils are being built with sanitizers enabled
# - to disable either set NOSAN=1 env variable or uncomment the line below
# NOSAN := 1

include ../phoenix-rtos-build/Makefile.common

.DEFAULT_GOAL := all

# provide common way for linking against hidapi for subcomponents
ifeq ($(UNAME_S),Linux)
  HIDAPI_LIB := -lhidapi-hidraw
else
  HIDAPI_LIB := $(shell pkg-config --libs hidapi)
endif

# read out all components
ALL_MAKES := $(wildcard */Makefile)
include $(ALL_MAKES)

# build all tools by default
DEFAULT_COMPONENTS := $(ALL_COMPONENTS)

# create generic targets
.PHONY: all install clean
all: $(DEFAULT_COMPONENTS)
install: $(patsubst %,%-install,$(DEFAULT_COMPONENTS))
clean: $(patsubst %,%-clean,$(ALL_COMPONENTS))
