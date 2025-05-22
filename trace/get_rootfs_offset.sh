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
disk_name="$(basename "${disk_path}")"

fdisk_output="$(fdisk -l "${disk_path}")"

start_ofs="$(echo "${fdisk_output}" | grep "${disk_name}1" | awk \{'print $3'\})"
sector_size="$(echo "${fdisk_output}" | grep "Sector size" | awk \{'print $7'\})"

echo $((start_ofs * sector_size))
