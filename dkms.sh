#!/bin/sh

# snd-usb-sinn7 kernel module dkms build script
#
# This file is responsible for adding the kernel source as dkms module.
#
# Copyright (C) 2017 Marc Streckfuß <marc.streckfuss@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

set -e

install_prereqs() {
    if ! dpkg-query -l build-essential | grep -q ii; then
        sudo apt-get install build-essential
    fi
    
    if ! dpkg-query -l linux-source | grep -q ii; then
        sudo apt-get install linux-source
    fi
    
    if ! dpkg-query -l dkms | grep -q ii; then
        sudo apt-get install dkms
    fi
    
    if ! dpkg-query -l linux-headers-`uname -r` | grep -q ii; then
        sudo apt-get install linux-headers-`uname -r`
    fi
}

update_repo() {
    git pull
}

extract_version() {
    VERSION=$( cat VERSION )
}

add_dkms() {
    # We should not hardcode /usr/src, since you can remap the destination folder and hand it on to dkms.
    # However users which reconfigured their system should be capable of altering two lines in this script.
    sudo mkdir -p /usr/src/snd-usb-sinn7-$VERSION
    cd src/
    sudo cp -R * /usr/src/snd-usb-sinn7-$VERSION
    sed "s/PACKAGE_VERSION=VER/PACKAGE_VERSION=$VERSION/" dkms.conf | sudo tee /usr/src/snd-usb-sinn7-$VERSION/dkms.conf > /dev/null
    cd ../
    sudo dkms add -m snd-usb-sinn7 -v $VERSION
}

build_dkms() {
    sudo dkms build -m snd-usb-sinn7 -v $VERSION
}

install_dkms() {
    sudo dkms install -m snd-usb-sinn7 -v $VERSION
}

# __main__
extract_version

if [ "$1" = "update" ]; then
    update_repo
    
    if ! dkms status -m snd-usb-sinn7 -v $VERSION | grep -q installed; then
        add_dkms
        build_dkms
        install_dkms
    fi
else
    install_prereqs
    
    #if ! dkms status -m snd-usb-sinn7 -v $VERSION | grep -q installed; then
        add_dkms
        build_dkms
        install_dkms
    #fi
fi

set +e
