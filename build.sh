#!/bin/sh
set -e
sudo make -j 8 CONFIG_DEBUG_INFO=y CONFIG_SND_USB=y CONFIG_SND_USB_AUDIO=m -C /lib/modules/`uname -r`/build M=$PWD V=1 modules
sudo make -j 8 CONFIG_DEBUG_INFO=y CONFIG_SND_USB=y CONFIG_SND_USB_AUDIO=m -C /lib/modules/`uname -r`/build M=$PWD V=1 modules_install

set +e
sudo depmod -a
sudo rmmod -f snd-usb-sinn7
sudo modprobe -v -C=$PWD snd-usb-sinn7
sudo alsa force-reload