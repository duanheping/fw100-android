#!/bin/bash
# echo "save fusion wireless files from root file system"
# echo "sdcard rootfs mounted on linux desktop card reader"

cp -r /media/rootfs/opt/fusion .
cp /media/rootfs/init.rc fusion/.

mkdir libs
cp /media/rootfs/system/lib/libril-fusion-100.so libs/.

mkdir bins
cp /media/rootfs/system/xbin/busybox bins/.
cp /media/rootfs/system/xbin/chat    bins/.
cp /media/rootfs/system/bin/radiooptions bins/.

mkdir etcs
cp -r /media/rootfs/system/etc/ppp etcs/.

# ppp enabled kernel image 0904
# $ sum uImage 12024  2765

mkdir images
cp /media/boot/uImage images/.

