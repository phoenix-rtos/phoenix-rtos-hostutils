#
# Makefile for phoenix-rtos-hostutils
#
# Copyright 2018-2021 Phoenix Systems
# Copyright 2001 Pawel Pisarczyk
#
# %LICENSE%
#

SIL ?= @
MAKEFLAGS += --no-print-directory

TARGET := host-generic-pc

include ../phoenix-rtos-build/Makefile.common
include ../phoenix-rtos-build/Makefile.$(TARGET_SUFF)

ifeq ($(UNAME_S),Linux)
	LDLIBS += -lhidapi-hidraw
else
	LDLIBS += `pkg-config --libs hidapi`
endif

.PHONY: clean
clean:
	@echo "rm -rf $(BUILD_DIR)"

ifneq ($(filter clean,$(MAKECMDGOALS)),)
	$(shell rm -rf $(BUILD_DIR))
endif

T1 := $(filter-out clean all,$(MAKECMDGOALS))
ifneq ($(T1),)
	include $(T1)/Makefile
.PHONY: $(T1)
$(T1): all
else
	include metaelf/Makefile
	include phoenixd/Makefile
	include psu/Makefile
	include psdisk/Makefile
	include syspagen/Makefile
endif
