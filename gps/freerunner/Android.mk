
# Use hardware GPS implementation if available.
# build this in .../hardware/libhardware_legacy/gps
#
LOCAL_CFLAGS           += -DHAVE_GPS_HARDWARE
# LOCAL_SHARED_LIBRARIES += $(BOARD_GPS_LIBRARIES)
LOCAL_SRC_FILES += gps/gps_freerunner.c

