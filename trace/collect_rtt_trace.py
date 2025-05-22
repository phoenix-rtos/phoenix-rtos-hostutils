#!/usr/bin/env python3
#
# Phoenix-RTOS
#
# Trace-over-RTT collector - runs OpenOCD with a given config and collects
# data from its RTT channel sockets
#
# NOTE: Assumes the config makes the OpenOCD expose the channels as follows:
# RTT_PORT_BASE + 2 * K     -> meta_channelK
# RTT_PORT_BASE + 2 * K + 1 -> event_channelK
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch

import os
import sys
import socket
import selectors
import errno
import time
import subprocess

RTT_PORT_BASE = 18023


class TraceOverRTTCollector:
    cores = []

    def connect_sockets(self, rtt_port_base):
        retries = 5
        while True:
            try:
                meta_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                meta_sock.connect(("localhost", rtt_port_base))
                meta_sock.setblocking(False)

                events_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                events_sock.connect(("localhost", rtt_port_base + 1))
                events_sock.setblocking(False)

                rtt_port_base += 2
                self.cores.append({"meta_sock": meta_sock, "events_sock": events_sock})
            except (OSError) as e:
                if e.errno == errno.ECONNREFUSED:
                    if len(self.cores) == 0:
                        if retries == 0:
                            print("Unable to connect to any RTT channel socket. Is OpenOCD configured correctly?")
                            raise
                        time.sleep(0.05)
                        retries -= 1
                        continue
                    print(f"Connected to {2 * len(self.cores)} channels")
                    return
                else:
                    raise

    def open_channel_files(self, output_dir):
        os.makedirs(output_dir, exist_ok=True)
        print(f"Saving traces to {os.path.realpath(output_dir)}")

        for (i, core) in enumerate(self.cores):
            meta_file = open(os.path.join(
                output_dir, f"channel_meta{i}"), "wb")
            events_file = open(os.path.join(
                output_dir, f"channel_event{i}"), "wb")
            core["meta_file"] = meta_file
            core["events_file"] = events_file

    def close_channel_files(self):
        for core in self.cores:
            core["meta_file"].close()
            core["events_file"].close()
        print("Files closed")

    wrote = dict()
    total = 0

    def read_from_socket(self, conn, mask, file):
        BUF_SIZE = 1024
        while True:
            try:
                data = conn.recv(BUF_SIZE)
            except (OSError) as e:
                if e.errno == errno.EAGAIN:
                    break
                else:
                    raise
            else:
                if not data:
                    break
                file.write(data)
                self.wrote[file.name] += len(data)
                self.total += len(data)

    def init_stats(self):
        for core in self.cores:
            self.wrote[core["meta_file"].name] = 0
            self.wrote[core["events_file"].name] = 0

    def register_sockets(self):
        sel = selectors.DefaultSelector()
        for core in self.cores:
            sel.register(core["events_sock"], selectors.EVENT_READ,
                         (self.read_from_socket, core["events_file"]))
            sel.register(core["meta_sock"], selectors.EVENT_READ,
                         (self.read_from_socket, core["meta_file"]))
        return sel

    def poll(self, sel):
        try:
            print("Ready to gather events. Do ^C when the trace has finished")

            last = time.time()
            last_total = self.total
            rate_kbps = 0
            status_printed = False

            while True:
                events_sock = sel.select()
                for key, mask in events_sock:
                    (callback, file) = key.data
                    callback(key.fileobj, mask, file)

                now = time.time()
                if now - last > 0.1:
                    rate_kbps = ((self.total - last_total) /
                                 (now - last)) / 1024
                    last = now
                    last_total = self.total

                    if status_printed:
                        for _ in range(len(self.wrote) + 1):
                            sys.stdout.write("\x1b[1A\x1b[2K")

                    print(f"Rate: {rate_kbps:.2f} KB/s")
                    for (filename, w) in self.wrote.items():
                        print(f"{os.path.basename(filename)}: {
                              w / 1024:.2f} KB ")
                        status_printed = True
        except KeyboardInterrupt:
            print("")

    def run(self, ocd_config, output_dir):
        p = subprocess.Popen(["openocd", "-f", ocd_config])
        print("OpenOCD started")
        try:
            self.connect_sockets(RTT_PORT_BASE)
            self.open_channel_files(output_dir)
            try:
                self.init_stats()
                sel = self.register_sockets()
                self.poll(sel)
            finally:
                self.close_channel_files()
        finally:
            p.terminate()
            print("OpenOCD stopped")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} OPENOCD_CONFIG OUTPUT_DIR")
        sys.exit(1)

    ocd_config = sys.argv[1]
    output_dir = sys.argv[2]

    c = TraceOverRTTCollector()
    c.run(ocd_config, output_dir)


if __name__ == "__main__":
    main()
