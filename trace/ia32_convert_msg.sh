#!/bin/bash
#
# Utility script for converting CTF trace to perfetto protobuf from ia32-generic-qemu image
#
# Usage: ./ia32_convert.sh DISK_IMG_PATH ROOTFS_CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]
# Options:
#		-t - run perfetto in browser with local trace processing acceleration (may
#		be useful for large traces)
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch

set -e

if [ "$#" -eq 0 ]; then
	echo "Usage: ./$(basename "$0") DISK_IMG_PATH ROOTFS_CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]"
	echo "Options:"
	echo "  -t:  run perfetto in browser with local trace processing acceleration (may be useful for large traces)"
	echo "Example: ./$(basename "$0") ../../_boot/ia32-generic-qemu/hd0.disk /trace output.pftrace"
	exit 1
fi

b_log() {
	echo -e "\033[1;33m$1\033[0m"
}

DISK_PATH="${1?no disk path given}"
CTF_DIR_PATH="${2?No ctf dir path given}"
METADATA_FILE_PATH="${3?No metadata path given}"
OUTPUT_SVG="${4?No output given}"
OPT="${5}"

b_log "gathering trace from disk image"

loop_dev="$(udisksctl loop-setup -f "${DISK_PATH}" -o $((4096 * 512)) | awk 'NF{ print $NF }' | sed 's/\.//g')"
mounted_rootfs_path="$(udisksctl mount -b "${loop_dev}" | awk 'NF{ print $NF }' | sed 's/\.//g')"

echo "${DISK_PATH} mounted to ${mounted_rootfs_path} (loop_dev=${loop_dev})"

function cleanup {
	udisksctl unmount -b "${loop_dev}"
	udisksctl loop-delete -b "${loop_dev}"
	echo "unmounted ${mounted_rootfs_path}, deleted loop_dev=${loop_dev}"
}

trap cleanup EXIT

./convert_msg.sh "${mounted_rootfs_path}/${CTF_DIR_PATH}" "${METADATA_FILE_PATH}" "${OUTPUT_SVG}"
