#
# Makefile for HIL sensor scenario sender and generator
#

HILSIM_DIR := $(call my-dir)

NAME := hilsim
LOCAL_DIR := $(HILSIM_DIR)
LOCAL_SRCS := hilsim.c
DEP_LIBS := libhostutils-common
LIBS := libsha256

include $(binary.mk)

NAME := hilgen
LOCAL_DIR := $(HILSIM_DIR)
LOCAL_SRCS := hilgen.c
LIBS := libsha256
LOCAL_LDLIBS := -lm

include $(binary.mk)


NAME := uart_latency
LOCAL_DIR := $(HILSIM_DIR)
LOCAL_SRCS := uart_latency.c
LOCAL_LDLIBS := -lm

include $(binary.mk)
