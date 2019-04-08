make ARCH=arm CROSS_COMPILE=/home/anna/Development/rpi-tools/arm-bcm2708/arm-bcm2708hardfp-linux-gnueabi/bin/arm-bcm2708hardfp-linux-gnueabi- bcm2709_defconfig
make ARCH=arm CROSS_COMPILE=/home/anna/Development/rpi-tools/arm-bcm2708/arm-bcm2708hardfp-linux-gnueabi/bin/arm-bcm2708hardfp-linux-gnueabi- -j10 LOCALVERSION=-rpi-thesis zImage modules dtbs
