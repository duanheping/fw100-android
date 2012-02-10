# Copyright 2006 The Android Open Source Project

# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE:= libril-fusion-100
LOCAL_MODULE_TAGS := debug

LOCAL_SRC_FILES:= \
    fw100-ril.c	\
    fw100-ril-rqst.c \
    fw100-ril-data.c \
    fw100-ril-timer.c \
    fw100-ril-utils.c \
    fw100-ril-gps.c \
    atchannel.c \
    misc.c \
    at_tok.c \
    rilinfo.c

LOCAL_SHARED_LIBRARIES := \
    libcutils libutils libnetutils libril

# for asprinf
LOCAL_CFLAGS := -D_GNU_SOURCE

LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

ifeq ($(TARGET_DEVICE),sooner)
  LOCAL_CFLAGS += -DOMAP_CSMI_POWER_CONTROL -DUSE_TI_COMMANDS
endif

ifeq ($(TARGET_DEVICE),surf)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

ifeq ($(TARGET_DEVICE),dream)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

ifeq (foo,foo)
  #build shared library
  LOCAL_SHARED_LIBRARIES += \
      libcutils libutils
  LOCAL_LDLIBS += -lpthread
  LOCAL_CFLAGS += -DRIL_SHLIB
  LOCAL_MODULE:= libril-fusion-100
  LOCAL_PRELINK_MODULE := false
  include $(BUILD_SHARED_LIBRARY)
else
  #build executable
  LOCAL_SHARED_LIBRARIES += \
      libril
  LOCAL_MODULE:= libril-fusion-100
  LOCAL_PRELINK_MODULE := false
  include $(BUILD_EXECUTABLE)
endif
