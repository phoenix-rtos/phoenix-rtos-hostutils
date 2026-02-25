#!/usr/bin/env python3

# Phoenix-RTOS
#
# CTF to Perfetto trace protobuf converter
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch
#

import sys
from argparse import ArgumentParser

from src.ctf_to_proto import Emitter


def main():
    parser = ArgumentParser()
    parser.add_argument("syscalls_path", help="path to list of syscalls")
    parser.add_argument("ctf_path", help="path to CTF folder to convert")
    parser.add_argument("output_path", help="path where to save the conversion result")

    if len(sys.argv) == 1:
        parser.print_help()
    args = parser.parse_args(sys.argv[1:])

    e = Emitter(args.syscalls_path)

    e.convert(args.ctf_path, args.output_path)


if __name__ == "__main__":
    main()
