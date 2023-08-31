#
# Makefile for Phoenix-RTOS psu (Phoenix Serial Uploader)
#
# Copyright 2019 Phoenix Systems
#

NAME := psu
LOCAL_DIR := $(call my-dir)
SRCS := $(wildcard $(LOCAL_DIR)*.c)
DEP_LIBS := libhostutils-common
LOCAL_LDLIBS := $(HIDAPI_LIB)

include $(binary.mk)
