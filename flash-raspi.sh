sudo make ARCH=arm CROSS_COMPILE=/home/anna/Development/rpi-tools/arm-bcm2708/arm-bcm2708hardfp-linux-gnueabi/bin/arm-bcm2708hardfp-linux-gnueabi- INSTALL_MOD_PATH=/run/media/anna/rootfs modules_install
sudo cp arch/arm/boot/zImage /run/media/anna/boot/thesiskernel.img
sudo cp arch/arm/boot/dts/*.dtb /run/media/anna/boot
sudo cp arch/arm/boot/dts/overlays/*.dtb* /run/media/anna/boot/overlays/
sudo cp arch/arm/boot/dts/overlays/README /run/media/anna/boot/overlays/
