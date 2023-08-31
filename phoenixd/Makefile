#
# Makefile for Phoenix-RTOS phoenixd (Phoenix daemon)
#
# Copyright 2019 Phoenix Systems
#

NAME := phoenixd
LOCAL_DIR := $(call my-dir)
SRCS := $(wildcard $(LOCAL_DIR)*.c)
DEP_LIBS := libhostutils-common
LOCAL_LDLIBS := $(HIDAPI_LIB)

include $(binary.mk)
