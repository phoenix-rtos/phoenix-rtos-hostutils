#!/usr/bin/env python3

import time
import bt2
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

from perfetto_trace_pb2 import TrackEvent


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


class Plotter:
    data = {}

    def emit_event(
        self,
        msg: bt2._EventMessageConst,
        name: str,
        phase: TrackEvent.Type,
    ):
        name = msg.event.name
        size = msg.event.payload_field['size']
        cycles = msg.event.payload_field['cycles']

        if name not in self.data:
            self.data[name] = []

        self.data[name].append((size, cycles))

        print(f"{name=} {size=} {cycles=}")

    def read_events(self, msg):
        self.emit_event(msg, msg.event.name, TrackEvent.Type.TYPE_INSTANT)

    def convert(self, path, output_path):
        eprint("converting CTF to perfetto")

        start = time.time()

        self.dest = open(output_path, "wb")

        msg_it = bt2.TraceCollectionMessageIterator(path)

        for msg in msg_it:
            if hasattr(msg, "event"):
                self.read_events(msg)

        self.dest.close()

        print(f"{len(self.data['msg_send'])}")

        fig, axs = plt.subplots(1, 3, figsize=(16, 6))

        i = 0

        for call, data in self.data.items():
            sizes, cycles = zip(*data)

            ax = axs[i]
            i += 1

            ax.scatter(sizes, cycles, color='blue', s=1)

            ax.set_xlabel('size [B]')
            ax.set_ylabel('cpu cycles')
            ax.set_title(call)

            if False:
                ax.xaxis.set_major_formatter(ScalarFormatter(useMathText=False))
                ax.ticklabel_format(style='plain', axis='x')

                ax.yaxis.set_major_formatter(ScalarFormatter(useMathText=False))
                ax.ticklabel_format(style='plain', axis='y')

            # Grid for better readability
            ax.grid(True, linestyle='--', alpha=0.5)

        plt.show()

        stop = time.time()
        eprint(f"finished in {stop - start:.2f} s")


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(
            "usage: " + sys.argv[0] + " [ctf path] [output path]\n")
        sys.exit(1)

    ctf_path = sys.argv[1]
    output_path = sys.argv[2]

    e = Plotter()

    e.convert(ctf_path, output_path)


if __name__ == "__main__":
    main()
