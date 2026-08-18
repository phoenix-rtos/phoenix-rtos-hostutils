#
# Host utils
#
# dump_chans - GDB command for dumping trace straight from the kernel buffers
#
# Usage:
# 1. Connect to the target with kernel symbols loaded
# 2. Load this script (`source phoenix-rtos-hostutils/trace/dump_chans.py`)
# 3. Start the trace, break when appropriate
# 4. Run `dump_chans <DEST_DIR>`
# 5. Run convert.sh as usual with <DEST_DIR> as trace source
#
# Copyright 2026 Phoenix Systems
# Author: Adam Greloch
#
# SPDX-License-Identifier: BSD-3-Clause
#

import gdb
import os


class DumpChannelsCommand(gdb.Command):
    """
    Dumps the buffer data from buffer_common.chans to channel_metaX and channel_eventX files.
    Usage: dump_chans <destination_directory>
    """

    def __init__(self):
        super(DumpChannelsCommand, self).__init__("dump_chans", gdb.COMMAND_USER)

    def invoke(self, argument, from_tty):
        try:
            argv = gdb.string_to_argv(argument)
            if not argv:
                print("Error: Missing destination directory.")
                print("Usage: dump_chans <destination_directory>")
                return

            dest_dir = argv[0]

            os.makedirs(dest_dir, exist_ok=True)

            buffer_common = gdb.parse_and_eval("buffer_common")
            chans = buffer_common["chans"]
            nchans = int(buffer_common["nchans"])

            inf = gdb.selected_inferior()

            print(f"Found {nchans} channels to dump into '{dest_dir}'.")

            for i in range(nchans):
                chan = chans[i]
                cbuffer = chan["buffer"]

                data_ptr = cbuffer["data"]
                full = int(cbuffer["full"])
                sz = int(cbuffer["w"])

                if full == 1:
                    print(f"[{i}] WARN: cbuffer full")

                channel_type = "meta" if (i % 2) == 0 else "event"
                channel_idx = i // 2

                filename = f"channel_{channel_type}{channel_idx}"
                filepath = os.path.join(dest_dir, filename)

                if int(data_ptr) == 0:
                    print(f"[{i}] Skipping {filename}: data pointer is NULL")
                    continue

                if sz <= 0:
                    print(f"[{i}] Skipping {filename}: size is {sz}")
                    continue

                memory_view = inf.read_memory(data_ptr, sz)
                with open(filepath, "wb") as f:
                    f.write(memory_view)

                print(f"[{i}] Dumped {sz} bytes at {data_ptr} -> {filepath}")

        except gdb.error as e:
            print(f"GDB Error: {e}")
        except Exception as e:
            print(f"Python Error: {e}")


# Register the command with GDB
DumpChannelsCommand()
