#!/bin/bash
#
# Script for calculating rootfs partition start offset from disk image. Useful
# when mounting the QEMU disk image
#
# Example:
#  DISK_PATH="../../_boot/ia32-generic-qemu/hd0.disk"
#  OFS="$(./get_rootfs_offset.sh "${DISK_PATH}")"
#  # The offset can be then passed to `mount` like so:
#  sudo mount -o loop,offset="${OFS}" "${DISK_PATH}" /mnt
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch

set -e
disk_path="${1?}"
parted -ms "${disk_path}" unit B print | grep '^1:' | cut -d: -f2 | sed 's/B$//'
