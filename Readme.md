# Sinn7 Status 24|96 USB Audio Driver for Linux

> The following software is not supported or related to Sinn7 (Global Distribution GmbH) 
> It is provided without guarantee and all rights/trademarks belong to their respective owners
> It is not authorized or endorsed by Global Distribution

## This driver is still work in progress
It specially does NOT comply to Linux Kernel Guidelines (Documentation/CodingStyle, scripts/checkpatch.pl).  
Pull Requests which address this are welcome.  
It also doesn't contain the support for multiple sample rates yet and the unloading seems bugged.  
**Warning**: This Kernel may crash or hang your system at any time and might lead to data loss or hardware failures.

### How to build the Kernel Module
Building the Kernel Module is actually really easy: You first need your recent Kernel Source.
For Debian/Ubuntu you can issue `sudo apt-get install linux-source` or `sudo apt-get install linux-source-4.8.0`
which will add `linux-source-4.8.0.tar.bz2` into `/usr/src`.  
Inside of this archive you have `debian` and `debian.master` which are control files, build files etc which we don't need here.  
Inside of this archive you also find `linux-source-4.8.0.tar.bz2`, extract that again to your workspace.  
  
Now change to `sound/usb`. Now copy the `src` folder of this project as `sinn7` folder under `sound/usb`. Also copy the `build.sh` into `sound/usb`.

**If you want to add the kernel module permanently to your OS**: Execute ./build.sh inside the `sound/usb` folder.  

**If you don't want to add the kernel module permanently to your OS**: Grab the 3rd Line of the Build Script (make .... modules) and execute it. It will build the .ko file into the `sinn7` folder. You just have to call sudo insmod `snd-usb-sinn7.ko` now to add it.
