# Sinn7 Status 24|96 USB Audio Driver for Linux

> The following software is not supported or related to Sinn7 (Global Distribution GmbH) 
> It is provided without guarantee and all rights/trademarks belong to their respective owners
> It is not authorized or endorsed by Global Distribution

## This driver is still work in progress
It specially does NOT comply to Linux Kernel Guidelines (Documentation/CodingStyle, scripts/checkpatch.pl).  
Pull Requests which address this are welcome.  
It also doesn't contain the support for multiple sample rates yet and the unloading seems bugged.  
**Warning**: This Kernel may crash or hang your system at any time and might lead to data loss or hardware failures.


## How to build the Kernel Module using DKMS (Recommended/Latest Version)
Building the Kernel Module using DKMS is even easier than the old build process. The good side of this is that each time a new kernel version is released/installed, then the snd-usb-sinn7 module is reloaded.  

This however would have the downside that (in case of `REMAKE_INITRD="yes"` in `src/dkms.conf`) the module gets included into your initramfs (so the module is directly included at boot time).
In case of a faulty driver, your system could refuse to boot until you regenerate the ramfs. You can turn on `REMAKE_INITRD="yes"` in the `dkms.conf` in `src/` to enforce this behavior. I don't know if the module will be loaded at boot time in the current configuration (`REMAKE_INITRD="no"`), so Issues/PRs explaining this are pretty welcome :) I _think_ you only need this for drivers that should be loaded early on (for network boot, usb hubs and the like).

In order to install the module as dkms, you generally have three options: Installing the `.deb` file provided by this repo's release section, using the script `dkms.sh` which comes with this repository or manually following the steps to add this repository as a dkms module.


### Using the .deb file
If you are running a debian based system like Debian, Ubuntu or related, you can just download the appropriate .deb file from this repos release section.
Since this file is always built for a specific kernel which you might not have, you need to ensure that your system has the required sources to build a new version locally:
Use `sudo apt-get install linux-source linux-headers-\`uname -r\` build-essential` to add those.
After that, you can use `sudo dpkg -i snd-usb-sinn7-dkms[...].deb` to install the driver and `sudo apt-get remove snd-usb-sinn7-dkms` to remove it again.  
Note: Currently I _guess_ we are unable to build using travis ci since it lacks the base drivers for sound (because it is a server).

### Using the DKMS Script
All you have to do here is to invoke `./dkms.sh` and wait for the process to complete. You are all set, great!  
You can also use this script to auto-update the driver to be in sync with the master branch. All you have to do is call `./dkms.sh update`.
Note that this requires root permissions (using sudo), so in case of an autostart, make it run as root or in some kind of interactive shell to query your password.
Internally it will simply perform a git pull and if the `VERSION` file changed (i.e. the current VERSION is not present on your system), it will be installed.  

### Manually build the dkms
First ensure that you have the needed packages (gcc, linux-source etc) -> `sudo apt-get install dkms build-essential linux-source linux-headers-\`uname -r\`` or `sudo apt-get install dkms build-essential linux-source-4.8.0 linux-headers-\`uname -r\``  
Create the module directory using `sudo mkdir /usr/src/snd-usb-sinn7-0.0.1/` (adjust version) and copy this repositories src/ folder there.
Then add the module to the DKMS tree using `dkms add -m snd-usb-sinn7 -v 0.0.1` (adjust version) and compile it using `dkms build -m snd-usb-sinn7 -v 0.0.1`. (Adjust version)  
When it is built you can execute `dkms install -m snd-usb-sinn7 -v 0.0.1` (adjust version) to install the module on your system.  


## How to build the Kernel Module (OLD, DEPRECATED)
Building the Kernel Module is actually really easy: You first need your recent Kernel Source.
For Debian/Ubuntu you can issue `sudo apt-get install linux-source linux-headers-\`uname -r\`` or `sudo apt-get install linux-source-4.8.0 linux-headers-\`uname -r\``
which will add `linux-source-4.8.0.tar.bz2` into `/usr/src`.  
Inside of this archive you have `debian` and `debian.master` which are control files, build files etc which we don't need here.  
Inside of this archive you also find `linux-source-4.8.0.tar.bz2`, extract that again to your workspace.  
  
Now change to `sound/usb`. Now copy the `src` folder of this project as `sinn7` folder under `sound/usb`. Also copy the `build.sh` into `sound/usb`.
Then you have to edit `sound/usb/Makefile` and add `sinn7/` to `obj-$(CONFIG_SND)`

**If you want to add the kernel module permanently to your OS**: Execute ./build.sh inside the `sound/usb` folder.  

**If you don't want to add the kernel module permanently to your OS**: Grab the 3rd Line of the Build Script (make .... modules) and execute it. It will build the .ko file into the `sinn7` folder. You just have to call sudo insmod `snd-usb-sinn7.ko` now to add it.
