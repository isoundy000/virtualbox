#!/bin/sh
#
# Oracle VM VirtualBox
# VirtualBox linux installation script unit test

#
# Copyright (C) 2007-2011 Oracle Corporation
#
# This file is part of VirtualBox Open Source Edition (OSE), as
# available from http://www.virtualbox.org. This file is free software;
# you can redistribute it and/or modify it under the terms of the GNU
# General Public License (GPL) as published by the Free Software
# Foundation, in version 2 as it comes in the "COPYING" file of the
# VirtualBox OSE distribution. VirtualBox OSE is distributed in the
# hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
#

#include installer-utils.sh

CERRS=0

echo "Testing udev rule generation for the \".run\" installer"

setup_test_input_install_udev ".run, udev-59" 59

udev_59_rules=`cat <<'UDEV_END'
KERNEL=="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="vboxusers", MODE="0660"
SUBSYSTEM=="usb_device", ACTION=="add", RUN+="/opt/VirtualBox/VBoxCreateUSBNode.sh $major $minor $attr{bDeviceClass}"
SUBSYSTEM=="usb", ACTION=="add", ENV{DEVTYPE}=="usb_device", RUN+="/opt/VirtualBox/VBoxCreateUSBNode.sh $major $minor $attr{bDeviceClass}"
SUBSYSTEM=="usb_device", ACTION=="remove", RUN+="/opt/VirtualBox/VBoxCreateUSBNode.sh --remove $major $minor"
SUBSYSTEM=="usb", ACTION=="remove", ENV{DEVTYPE}=="usb_device", RUN+="/opt/VirtualBox/VBoxCreateUSBNode.sh --remove $major $minor"
UDEV_END`

install_udev_output="`install_udev_run vboxusers 0660 /opt/VirtualBox`"
case "$install_udev_output" in
    "$udev_59_rules") ;;
    *)
        echo "Bad output for udev version 59.  Expected:"
        echo "$udev_59_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

setup_test_input_install_udev ".run, udev-55" 55

udev_55_rules=`cat <<'UDEV_END'
KERNEL=="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="vboxusers", MODE="0660"
UDEV_END`

install_udev_output="`install_udev_run vboxusers 0660 /opt/VirtualBox`"
case "$install_udev_output" in
    "$udev_55_rules") ;;
    *)
        echo "Bad output for udev version 55.  Expected:"
        echo "$udev_55_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

setup_test_input_install_udev ".run, udev-54" 54

udev_54_rules=`cat <<'UDEV_END'
KERNEL="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="root", MODE="0600"
UDEV_END`

install_udev_output="`install_udev_run root 0600 /usr/lib/virtualbox`"
case "$install_udev_output" in
    "$udev_54_rules") ;;
    *)
        echo "Bad output for udev version 54.  Expected:"
        echo "$udev_54_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

echo "Testing udev rule generation for the \"package\" installer"

setup_test_input_install_udev "package, udev-59" 59

udev_59_rules=`cat <<'UDEV_END'
KERNEL=="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="root", MODE="0600"
SUBSYSTEM=="usb_device", ACTION=="add", RUN+="/usr/share/virtualbox/VBoxCreateUSBNode.sh $major $minor $attr{bDeviceClass} vboxusers"
SUBSYSTEM=="usb", ACTION=="add", ENV{DEVTYPE}=="usb_device", RUN+="/usr/share/virtualbox/VBoxCreateUSBNode.sh $major $minor $attr{bDeviceClass} vboxusers"
SUBSYSTEM=="usb_device", ACTION=="remove", RUN+="/usr/share/virtualbox/VBoxCreateUSBNode.sh --remove $major $minor"
SUBSYSTEM=="usb", ACTION=="remove", ENV{DEVTYPE}=="usb_device", RUN+="/usr/share/virtualbox/VBoxCreateUSBNode.sh --remove $major $minor"
UDEV_END`

install_udev_output="`install_udev_package vboxusers`"
case "$install_udev_output" in
    "$udev_59_rules") ;;
    *)
        echo "Bad output for udev version 59.  Expected:"
        echo "$udev_59_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

setup_test_input_install_udev "package, udev-55" 55

udev_55_rules=`cat <<'UDEV_END'
KERNEL=="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="root", MODE="0600"
UDEV_END`

install_udev_output="`install_udev_package vboxusers`"
case "$install_udev_output" in
    "$udev_55_rules") ;;
    *)
        echo "Bad output for udev version 55.  Expected:"
        echo "$udev_55_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

setup_test_input_install_udev "package, udev-54" 54

udev_54_rules=`cat <<'UDEV_END'
KERNEL="vboxdrv", NAME="vboxdrv", OWNER="root", GROUP="root", MODE="0600"
UDEV_END`

install_udev_output="`install_udev_package root`"
case "$install_udev_output" in
    "$udev_54_rules") ;;
    *)
        echo "Bad output for udev version 54.  Expected:"
        echo "$udev_54_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev

setup_test_input_install_udev "package, no udev" 54
INSTALL_NO_UDEV=1

install_udev_output="`install_udev_package root`"
case "$install_udev_output" in
    "") ;;
    *)
        echo "Bad output for udev version 54.  Expected:"
        echo "$udev_54_rules"
        echo "Actual:"
        echo "$install_udev_output"
        CERRS="`expr "$CERRS" + 1`"
        ;;
esac

cleanup_test_input_install_udev
INSTALL_NO_UDEV=

echo "Done.  Error count $CERRS."
