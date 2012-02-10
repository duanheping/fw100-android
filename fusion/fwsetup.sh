#!/bin/sh
# set up Fusion Wireless CDMA LKM and related components

# LKM
insmod /opt/fusion/FWUSBModem.ko

sleep 1

echo "setting device permissions /dev/ttyUSB*"
chmod 666 /dev/ttyUSB0
chmod 666 /dev/ttyUSB1
chmod 666 /dev/ttyUSB2

# mknod that need only run once on persistent root file system
# pppd related
/system/xbin/busybox mknod /dev/ppp c 108 0
mkdir /var
mkdir /var/lock
# remove any stale locks
rm -f /var/lock/LCK*

# add netcfg usb0 gadget
ifconfig eth0 192.168.1.48 netmask 255.255.255.0 up

sleep 1

