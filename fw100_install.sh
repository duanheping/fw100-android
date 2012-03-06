#!/bin/bash
echo " "
echo "Update beagleboard android root file system using"
echo "sdcard rootfs mounted on linux desktop card reader"

echo " "
echo "NOTE: TI script mkmmc-android.sh must be run first"
echo "to install base root file system and directory structure."
echo "Then this script to install fusion wireless related files."
echo " "

TOP=`pwd`

# NOTE:  change this path if target sd card 
# uses something other than /media/rootfs...
ROOTFS=$TOP/out/target/product/tao3530/android_rootfs

# fw ril
mkdir $ROOTFS/opt
mkdir $ROOTFS/opt/fusion
# pppd 
mkdir $ROOTFS/var
mkdir $ROOTFS/var/lock

chmod 777 $ROOTFS/opt
chmod 777 $ROOTFS/opt/fusion
chmod 755 $ROOTFS/var
chmod 755 $ROOTFS/var/lock

# note: review fusion wireless changes to samples
# in directory fusion/init*.rc
# cp fusion/init.rc $ROOTFS/.
#chmod 755 $ROOTFS/init.rc

# don't need the fusion LKM if kernel build
# has fusion drivers built-in
cp -r $TOP/fw-android-1228A/fusion/FWUSBModem.ko $ROOTFS/opt/fusion/.
cp -r $TOP/fw-android-1228A/fusion/fwsetup.sh    $ROOTFS/opt/fusion/.

#cp $TOP/fw-android-1228A/libs/libril-fusion-100.so $ROOTFS/system/lib/.

cp $TOP/fw-android-1228A/bins/busybox      $ROOTFS/system/xbin/.
cp $TOP/fw-android-1228A/bins/chat         $ROOTFS/system/xbin/.

cp -r $TOP/fw-android-1228A/etcs/ppp $ROOTFS/system/etc/.
chmod 755 $ROOTFS/system/etc/ppp
chmod 755 $ROOTFS/system/etc/ppp/peers

# users kernel image must have PPP enabled.
# replace generic kernel image with ppp enabled build
#cp images/uImage /media/boot/uImage

echo " "
echo "Files Installed OK"
echo " "

