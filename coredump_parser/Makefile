#
# Makefile for Phoenix-RTOS coredump parser
#
# Copyright 2025 Phoenix Systems
#

NAME := coredump_parser
LOCAL_DIR := $(call my-dir)
SRCS := $(wildcard $(LOCAL_DIR)*.cpp)
LOCAL_LDLIBS := -lstdc++
SCRIPTS := $(wildcard $(LOCAL_DIR)*.py)


include $(binary.mk)


$(NAME)-scripts:
	mkdir -p $(PREFIX_BUILD)/scripts
	cp $(SCRIPTS) $(PREFIX_BUILD)/scripts

ALL_COMPONENTS += $(NAME)-scripts