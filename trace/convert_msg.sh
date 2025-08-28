#!/bin/bash
#
# Utility script for converting CTF trace to perfetto protobuf
#
# Requires: babeltrace2 python3-bt2 protobuf-compiler python3-protobuf
#
# Usage: ./convert.sh CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]
# Options:
#		-t - run perfetto in browser with local trace processing acceleration (may
#		be useful for large traces)
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch

if [ "$#" -eq 0 ]; then
	echo "Usage: ./$(basename "$0") CTF_DIR_PATH METADATA_PATH OUTPUT [OPTIONS]"
	echo "Options:"
	echo "  -t:  run perfetto in browser with local trace processing acceleration (may be useful for large traces)"
	echo "Example: ./$(basename "$0") my-ctf-trace ../../phoenix-rtos-kernel/perf/tsdl/metadata output.pftrace"
	exit 1
fi

if ! command -v babeltrace2 >/dev/null 2>&1; then
	echo "babeltrace2 not found"
	exit 1
fi

if ! command -v protoc >/dev/null 2>&1; then
	echo "protoc not found"
	exit 1
fi

b_log() {
	echo -e "\033[1;33m$1\033[0m"
}

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

source "${SCRIPT_DIR}/trim_event_stream.subr"

set -e

CTF_DIR_PATH="${1?No ctf dir path given}"
METADATA_FILE_PATH="${2?No metadata path given}"
OUTPUT_SVG="${3?No output given}"

TMP_DIR="${SCRIPT_DIR}/tmp"
TRACE_DIR="${TMP_DIR}/ctf-msg-$(date +%FT%T)"

b_log "copying CTF data streams"

mkdir -vp "${TRACE_DIR}"

cp -v "${CTF_DIR_PATH}"/* "${TRACE_DIR}"

echo -n "creating symlink to CTF metadata: "
ln -rvsf "${METADATA_FILE_PATH}" "${TRACE_DIR}"

b_log "adding stream context"

TMP_FILE="${TRACE_DIR}/tmp"

for stream in "${TRACE_DIR}"/channel_*; do
	filename="$(basename "${stream}")"
	cpu="${filename//[!0-9]/}"

	echo "${filename} size: $(du -h "${stream}" | cut -f 1), cpu: ${cpu}"

	# shellcheck disable=2059
	{
		printf "\x${cpu}"
		cat "${stream}"
	} >"${TMP_FILE}"
	mv "${TMP_FILE}" "${stream}"
done

b_log "trimming event streams"

trim_event_stream "${TRACE_DIR}"

CTF_TO_PROTO_DIR="${SCRIPT_DIR}/ctf_to_proto/"
PYTHON_SRC="${CTF_TO_PROTO_DIR}/src"

MSG_CTF="${PYTHON_SRC}/msg_ctf.py"

PROTO_SRC="${CTF_TO_PROTO_DIR}/proto"
PROTO_FILE_PATH="${PROTO_SRC}/perfetto_trace.proto"

PROTO_URL="https://github.com/google/perfetto/raw/refs/heads/main/protos/perfetto/trace/perfetto_trace.proto"

if [ ! -f "${PROTO_FILE_PATH}" ]; then
	b_log "preparing ctf_to_proto.py"
	mkdir -p "${PROTO_SRC}"
	wget "${PROTO_URL}" -P "${PROTO_SRC}"
	protoc --proto_path="${PROTO_SRC}" --python_out="${PYTHON_SRC}" "${PROTO_FILE_PATH}"
fi

b_log "converting using ${CTF_TO_PROTO}"

time "${MSG_CTF}" "${TRACE_DIR}" "${OUTPUT_SVG}"
