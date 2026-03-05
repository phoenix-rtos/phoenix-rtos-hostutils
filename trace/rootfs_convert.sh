#!/bin/bash
#
# Utility script for converting CTF trace to perfetto protobuf from ia32-generic-qemu image
#
# Usage: ./rootfs_convert.sh DISK_IMG_PATH ROOTFS_CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]
#
# Options are passed to ctf_to_proto (see: `python3 ctf_to_proto -h`).
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

if [ "$#" -eq 0 ]; then
	echo "Usage: ./$(basename "$0") DISK_IMG_PATH ROOTFS_CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]"
	echo ""
	echo "Example: ./$(basename "$0") ../../_boot/ia32-generic-qemu/hd0.disk /trace output.pftrace"
	echo ""
	echo "OPTIONS can be used to pass ctf_to_proto options:"
	python3 "${SCRIPT_DIR}/ctf_to_proto" -h
	exit 1
fi

b_log() {
	echo -e "\033[1;33m$1\033[0m"
}

DISK_PATH="${1?no disk path given}"
CTF_DIR_PATH="${2?No ctf dir path given}"
METADATA_FILE_PATH="${3?No metadata path given}"
OUTPUT_PFTRACE="${4?No output given}"
OPT="${5}"

b_log "gathering trace from disk image"

loop_dev="$(udisksctl loop-setup -f "${DISK_PATH}" -o "$(./get_rootfs_offset.sh "${DISK_PATH}")" | awk 'NF{ print $NF }' | sed 's/\.//g')"
mounted_rootfs_path="$(udisksctl mount -b "${loop_dev}" | awk 'NF{ print $NF }' | sed 's/\.//g')"

echo "${DISK_PATH} mounted to ${mounted_rootfs_path} (loop_dev=${loop_dev})"

function cleanup {
	udisksctl unmount -b "${loop_dev}"
	udisksctl loop-delete -b "${loop_dev}"
	echo "unmounted ${mounted_rootfs_path}, deleted loop_dev=${loop_dev}"
}

trap cleanup EXIT

./convert.sh "${mounted_rootfs_path}/${CTF_DIR_PATH}" "${METADATA_FILE_PATH}" "${OUTPUT_PFTRACE}" "${OPT}"
