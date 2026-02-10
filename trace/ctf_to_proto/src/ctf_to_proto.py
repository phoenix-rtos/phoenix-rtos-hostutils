#!/usr/bin/env python3

# Phoenix-RTOS
#
# CTF to Perfetto trace protobuf converter
#
# Copyright 2025 Phoenix Systems
# Author: Adam Greloch
#

import time
import bt2
import sys
from enum import Enum

import perfetto_trace_pb2
from perfetto_trace_pb2 import TrackEvent, CounterDescriptor


prtos_syscalls = (
    "debug",
    "sys_mmap",
    "sys_munmap",
    "sys_fork",
    "vforksvc",
    "exec",
    "spawnSyspage",
    "sys_exit",
    "sys_waitpid",
    "threadJoin",
    "getpid",
    "getppid",
    "gettid",
    "beginthreadex",
    "endthread",
    "nsleep",
    "phMutexCreate",
    "phMutexLock",
    "mutexTry",
    "mutexUnlock",
    "phCondCreate",
    "phCondWait",
    "condSignal",
    "condBroadcast",
    "resourceDestroy",
    "interrupt",
    "portCreate",
    "portDestroy",
    "portRegister",
    "msgSend",
    "msgRecv",
    "msgRespond",
    "lookup",
    "gettime",
    "settime",
    "keepidle",
    "platformctl",
    "wdgreload",
    "threadsinfo",
    "meminfo",
    "sys_perf_start",
    "sys_perf_read",
    "sys_perf_finish",
    "sys_perf_stop",
    "syspageprog",
    "va2pa",
    "signalHandle",
    "signalPost",
    "signalMask",
    "signalSuspend",
    "priority",
    "sys_read",
    "sys_write",
    "sys_open",
    "sys_close",
    "sys_link",
    "sys_unlink",
    "sys_fcntl",
    "sys_ftruncate",
    "sys_lseek",
    "sys_dup",
    "sys_dup2",
    "sys_pipe",
    "sys_mkfifo",
    "sys_chmod",
    "sys_fstat",
    "sys_fsync",
    "sys_accept",
    "sys_accept4",
    "sys_bind",
    "sys_connect",
    "sys_gethostname",
    "sys_getpeername",
    "sys_getsockname",
    "sys_getsockopt",
    "sys_listen",
    "sys_recvfrom",
    "sys_sendto",
    "sys_recvmsg",
    "sys_sendmsg",
    "sys_socket",
    "sys_socketpair",
    "sys_shutdown",
    "sys_sethostname",
    "sys_setsockopt",
    "sys_ioctl",
    "sys_futimens",
    "sys_poll",
    "sys_tkill",
    "sys_setpgid",
    "sys_getpgid",
    "sys_setpgrp",
    "sys_getpgrp",
    "sys_setsid",
    "sys_spawn",
    "release",
    "sbi_putchar",
    "sbi_getchar",
    "sigreturn",
    "sys_mprotect",
    "sys_statvfs",
    "sys_uname",
)


class SyntheticEvents(Enum):
    INTERRUPT = "interrupt"
    IN_LOCK_SET = "lockSet"
    LOCKED = "locked"
    SYSCALL = "syscall"
    SCHED = "sched"
    RUNNABLE = "runnable"


prtos_synthetic_events = {
    SyntheticEvents.INTERRUPT: ("interrupt_enter", "interrupt_exit"),
    SyntheticEvents.IN_LOCK_SET: ("lock_set_enter", "lock_set_exit"),
    SyntheticEvents.LOCKED: ("lock_set_acquired", "lock_clear"),
    SyntheticEvents.SYSCALL: ("syscall_enter", "syscall_exit"),
    SyntheticEvents.SCHED: ("sched_enter", "sched_exit"),
    SyntheticEvents.RUNNABLE: ("thread_waking", "thread_scheduling"),
}


def lower(x):
    if isinstance(x, str) or isinstance(x, int) or isinstance(x, float):
        return x
    if isinstance(x, dict) or isinstance(x, bt2._StructureFieldConst):
        return {lower(k): lower(v) for k, v in x.items()}
    if isinstance(x, bt2._BoolValueConst) \
            or isinstance(x, bt2._BoolFieldConst):
        return bool(x)
    if isinstance(x, bt2._EnumerationFieldConst):
        return repr(x)
    if isinstance(x, bt2._IntegerValueConst) \
            or isinstance(x, bt2._IntegerFieldConst):
        return int(x)
    if isinstance(x, bt2._RealValueConst) \
            or isinstance(x, bt2._RealFieldConst):
        return float(x)
    if isinstance(x, bt2._StringValueConst) \
            or isinstance(x, bt2._StringFieldConst):
        return str(x)
    raise ValueError("Unexpected value from trace", x)


def put(str):
    sys.stdout.write(str)


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


BATCH_SIZE = 100000

REAL_PROC_ID_BASE = 0
CPU_PROC_ID = 100000

PACKET_SEQ = 1111222223

MERGE_PRIORITIES = True

UNKNOWN_TID = 999999999
KERNEL_TID = -1

uid = 42  # should be non-zero


def next_uid():
    global uid

    res = uid
    uid += 1
    return res


class Emitter:
    base_time_us = None

    synthetic_begin = {}
    synthetic_end = {}

    initial_metadata_emitted = False

    tid_to_events_track_uid = dict()
    tid_to_sched_track_uid = dict()
    tid_to_prio_track_uid = dict()

    pid_to_uid = dict()
    pid_to_prio_uid = dict()

    prev_cpu_event = dict()
    prev_running_thread_event = dict()

    prev_cpu = dict()

    ongoing_events: dict[list[dict]] = dict()

    dest = None

    cpus_uid = None
    cpu_uids = dict()
    cpu_flow_ids = dict()

    kernel_uid = None
    kernel_cpu_uids = dict()

    priorities_uid = None

    lock_names = dict()

    last_flush = None
    events_total = 0

    tid_curr_prio = dict()

    warn_unknown_threads = False

    def __init__(self):
        for synthetic, (begin, end) in prtos_synthetic_events.items():
            self.synthetic_begin[begin] = []
            self.synthetic_end[end] = []

        for synthetic, (begin, end) in prtos_synthetic_events.items():
            self.synthetic_begin[begin].append(synthetic)
            self.synthetic_end[end].append(synthetic)

    @staticmethod
    def tid_or_kernel(event):
        if "tid" in event.payload_field:
            return lower(event.payload_field["tid"])
        else:
            return KERNEL_TID

    def event_us(self, msg):
        us = msg.default_clock_snapshot.value

        if self.base_time_us is None:
            # do assertions once - the clock config doesn't change in our case
            assert msg.default_clock_snapshot.clock_class.name == "monotonic"
            assert msg.default_clock_snapshot.clock_class.frequency == 1e6
            self.base_time_us = us

        return (us - self.base_time_us) * 1000

    current_trace = perfetto_trace_pb2.Trace()

    def flush_current_trace(self):
        event_count = len(self.current_trace.packet)

        self.dest.write(self.current_trace.SerializeToString())
        self.current_trace = perfetto_trace_pb2.Trace()

        now = time.time()
        self.events_total += event_count
        delta = now - self.last_flush
        eprint(f"emitted {self.events_total} events"
               f" ({event_count / delta:.2f} events/s)")
        self.last_flush = now

    def print_trace_packets(self, packets):
        for packet in packets:
            self.current_trace.packet.append(packet)

        if (len(self.current_trace.packet) >= BATCH_SIZE):
            self.flush_current_trace()

    def add_new_thread(self, **kwargs):
        kwargs = {**{'ts': 0},  **kwargs}

        tid = kwargs['tid']
        pid = kwargs['pid']
        prio = kwargs['prio']

        # may be tempting to use tid as track_uid, but in case of synthetic
        # tracks this could create unnecessary mess
        uid = next_uid()
        sched_uid = next_uid()
        events_uid = next_uid()
        prio_uid = next_uid()

        self.tid_to_events_track_uid[tid] = events_uid
        self.tid_to_sched_track_uid[tid] = sched_uid
        self.tid_to_prio_track_uid[tid] = prio_uid

        self.ongoing_events[tid] = dict()

        packets = []

        if pid not in self.pid_to_uid:
            name = lower(kwargs['name'])
            packet = perfetto_trace_pb2.TracePacket()
            process_uid = next_uid()
            packet.track_descriptor.uuid = process_uid
            packet.track_descriptor.process.pid = pid
            packet.track_descriptor.process.process_name = \
                f"'{name}'"
            packets.append(packet)
            self.pid_to_uid[pid] = process_uid

            if not MERGE_PRIORITIES:
                pid_prio_uid = next_uid()
                packet = perfetto_trace_pb2.TracePacket()
                packet.track_descriptor.uuid = pid_prio_uid
                packet.track_descriptor.parent_uuid = self.priorities_uid
                packet.track_descriptor.name = f"'{name}' {pid}"
                packets.append(packet)
                self.pid_to_prio_uid[pid] = pid_prio_uid

            eprint(f"add process: '{name}' {pid=}")

        root_packet = perfetto_trace_pb2.TracePacket()
        root_packet.track_descriptor.uuid = uid
        root_packet.track_descriptor.thread.pid = pid
        root_packet.track_descriptor.thread.tid = tid

        sched_packet = perfetto_trace_pb2.TracePacket()
        sched_packet.track_descriptor.uuid = sched_uid
        sched_packet.track_descriptor.parent_uuid = uid
        sched_packet.track_descriptor.name = "sched"

        events_packet = perfetto_trace_pb2.TracePacket()
        events_packet.track_descriptor.uuid = events_uid
        events_packet.track_descriptor.parent_uuid = uid
        events_packet.track_descriptor.name = "events"

        prio_packet = perfetto_trace_pb2.TracePacket()
        prio_packet.track_descriptor.uuid = prio_uid
        prio_packet.track_descriptor.parent_uuid = \
            uid if MERGE_PRIORITIES else self.pid_to_prio_uid[pid]
        prio_packet.track_descriptor.name = "prio"
        prio_packet.track_descriptor.counter.unit_name = "prio"

        packets += [root_packet, sched_packet, events_packet, prio_packet]

        self.tid_curr_prio[tid] = prio

        self.print_trace_packets(packets)

        eprint(f"add thread: {tid=} {pid=} {prio=}")

    def add_ongoing_event(self, tid, packet):
        event_name = packet.track_event.name

        if event_name not in self.ongoing_events[tid]:
            self.ongoing_events[tid][event_name] = []

        self.ongoing_events[tid][event_name].append(packet)

    def pop_ongoing_event(self, tid, packet):
        key = packet.track_event.name

        if key not in self.ongoing_events[tid] \
                or not self.ongoing_events[tid][key]:
            return None

        return self.ongoing_events[tid][key].pop()

    def end_ongoing_events(self, tid, ts):
        for packets in self.ongoing_events[tid].values():
            for packet in packets:
                packet.track_event.type = TrackEvent.Type.TYPE_SLICE_END
                packet.timestamp = ts
                self.print_trace_packets([packet])

        self.ongoing_events[tid].clear()

    def emit_initial_metadata(self, event):
        # emit "CPUs" track - its subtracks denote CPUs and show
        # which kernel thread is currently scheduled on which CPU

        packets = []

        self.cpus_uid = next_uid()
        cpus_packet = perfetto_trace_pb2.TracePacket()
        cpus_packet.track_descriptor.uuid = self.cpus_uid
        cpus_packet.track_descriptor.name = "CPUs"
        packets.append(cpus_packet)

        self.kernel_uid = next_uid()
        kernel_packet = perfetto_trace_pb2.TracePacket()
        kernel_packet.track_descriptor.uuid = self.kernel_uid
        kernel_packet.track_descriptor.name = "KERNEL"
        packets.append(kernel_packet)

        if not MERGE_PRIORITIES:
            self.priorities_uid = next_uid()
            priorities_packet = perfetto_trace_pb2.TracePacket()
            priorities_packet.track_descriptor.uuid = self.priorities_uid
            priorities_packet.track_descriptor.name = "Priorities"
            packets.append(priorities_packet)

        self.print_trace_packets(packets)

        # Create dummy thread for stray threads
        self.threads[UNKNOWN_TID] = {'pid': 999999999, 'name': 'UNKNOWN', 'prio':
                                     999, 'ts': 0}

        # Initialize kernel thread by hand without adding it to self.threads
        # The kernel has its own special (not thread-like) track
        self.ongoing_events[KERNEL_TID] = dict()

    def emit_kernel_cpu_if_new(self, cpu):
        if cpu not in self.kernel_cpu_uids:
            self.kernel_cpu_uids[cpu] = next_uid()

            kernel_cpu_packet = perfetto_trace_pb2.TracePacket()
            kernel_cpu_packet.track_descriptor.uuid = self.kernel_cpu_uids[cpu]
            kernel_cpu_packet.track_descriptor.parent_uuid = self.kernel_uid
            kernel_cpu_packet.track_descriptor.name = f"CPU {cpu}"

            self.print_trace_packets([kernel_cpu_packet])

    def emit_virtual_cpu_if_new(self, cpu):
        if cpu not in self.cpu_uids:
            self.cpu_uids[cpu] = next_uid()
            self.cpu_flow_ids[cpu] = next_uid()

            cpu_packet = perfetto_trace_pb2.TracePacket()
            cpu_packet.track_descriptor.uuid = self.cpu_uids[cpu]
            cpu_packet.track_descriptor.parent_uuid = self.cpus_uid
            cpu_packet.track_descriptor.name = f"CPU {cpu}"

            self.print_trace_packets([cpu_packet])

    def update_cpu_virtual_thread(self, msg: bt2._EventMessageConst, cpu):
        tid = self.tid_or_kernel(msg.event)

        tname = lower(f"{self.get_thread(tid)['name']} {tid}")

        self.emit_virtual_cpu_if_new(cpu)

        if cpu not in self.prev_cpu_event or \
                self.prev_cpu_event[cpu].track_event.name != tname:

            packets = []

            if cpu in self.prev_cpu_event:
                prev_packet = self.prev_cpu_event[cpu]
                prev_packet.timestamp = self.event_us(msg)
                prev_packet.track_event.type = TrackEvent.Type.TYPE_SLICE_END
                packets.append(prev_packet)

            packet = perfetto_trace_pb2.TracePacket()
            packet.timestamp = self.event_us(msg)
            packet.track_event.type = TrackEvent.Type.TYPE_SLICE_BEGIN
            packet.track_event.name = tname
            packet.track_event.track_uuid = self.cpu_uids[cpu]
            packet.trusted_packet_sequence_id = PACKET_SEQ
            packets.append(packet)

            self.prev_cpu_event[cpu] = packet

            self.print_trace_packets(packets)

            self.update_running_thread(msg, cpu)

    def update_running_thread(self, msg: bt2._EventMessageConst, cpu):
        tid = self.tid_or_kernel(msg.event)

        packets = []

        if cpu in self.prev_running_thread_event:
            packet = self.prev_running_thread_event[cpu]

            packet.timestamp = self.event_us(msg)
            packet.track_event.type = TrackEvent.Type.TYPE_SLICE_END

            packets.append(packet)

        packet = perfetto_trace_pb2.TracePacket()

        packet.timestamp = self.event_us(msg) + 1
        packet.track_event.type = TrackEvent.Type.TYPE_SLICE_BEGIN
        packet.track_event.name = f"running:cpu{cpu}"
        packet.track_event.track_uuid = self.tid_to_sched_track_uid[tid]
        packet.track_event.flow_ids.append(self.cpu_flow_ids[cpu])
        packet.trusted_packet_sequence_id = PACKET_SEQ

        packets.append(packet)

        self.prev_running_thread_event[cpu] = packet

        self.print_trace_packets(packets)

    def get_lock_name(self, msg):
        lock_id = int(msg.event.payload_field["lid"])
        if lock_id in self.lock_names:
            return self.lock_names[lock_id]
        else:
            return f"0x{lock_id:x}"

    def emit_prio_change(
        self,
        tid: int,
        prio: int,
        ts: int
    ):
        packet = perfetto_trace_pb2.TracePacket()
        packet.timestamp = ts
        packet.track_event.type = TrackEvent.Type.TYPE_COUNTER
        packet.track_event.counter_value = prio
        packet.track_event.track_uuid = self.tid_to_prio_track_uid[tid]
        packet.trusted_packet_sequence_id = PACKET_SEQ
        return packet

    first_event = True

    tid_emitted = set()
    threads = dict()

    def get_thread(self, tid):
        if tid not in self.threads:
            tid = UNKNOWN_TID
            self.warn_unknown_threads = True
        return self.threads[tid]

    def emit_event(
        self,
        msg: bt2._EventMessageConst,
        name: str | SyntheticEvents,
        phase: TrackEvent.Type,
    ):
        if not self.initial_metadata_emitted:
            self.emit_initial_metadata(msg.event)
            self.initial_metadata_emitted = True

        event = msg.event
        args = dict(event.payload_field)
        tid = self.tid_or_kernel(event)
        ts = self.event_us(msg)
        track_uuid = None
        cpu = event['cpu']
        update_cpu = False
        flow_id = None
        shift_by_ns = False

        if name == "thread_create":
            self.threads[tid] = {'pid': args['pid'], 'name': args['name'], 'prio':
                                 args['prio'], 'ts': ts}
            return  # meta event

        if tid != KERNEL_TID and tid not in self.tid_emitted:
            t = self.get_thread(tid)
            self.add_new_thread(
                tid=tid, pid=t['pid'], name=t['name'],
                prio=t['prio'], ts=ts)
            event = self.emit_prio_change(
                tid, self.tid_curr_prio[tid], ts)
            self.print_trace_packets([event])
            self.tid_emitted.add(tid)

        event_name = name.value if type(name) is SyntheticEvents else name

        match name:
            case SyntheticEvents.SYSCALL:
                n = args["n"]
                event_name = "syscall:" + prtos_syscalls[n]
            case SyntheticEvents.INTERRUPT:
                irq = args["irq"]
                event_name = f"irq:{irq}"
            case SyntheticEvents.IN_LOCK_SET:
                lock_name = self.get_lock_name(msg)
                event_name = "lockSet:" + lock_name
            case SyntheticEvents.LOCKED:
                lock_name = self.get_lock_name(msg)
                event_name = "locked:" + lock_name
                flow_id = int(args["lid"])
                if phase == TrackEvent.Type.TYPE_SLICE_BEGIN:
                    # WORKAROUND: perfetto doesn't like IN_LOCK_SET END
                    # having the same ts as LOCKED BEGIN, resulting in
                    # LOCKED event not showing up on the timeline
                    shift_by_ns = True
            case SyntheticEvents.RUNNABLE:
                track_uuid = self.tid_to_sched_track_uid[tid]
                if phase == TrackEvent.Type.TYPE_SLICE_END:
                    update_cpu = True
            case "lock_name":
                lock_id = int(args["lid"])
                lock_name = str(args["name"])
                self.lock_names[lock_id] = lock_name
                return  # meta event
            case "thread_priority":
                self.print_trace_packets(
                    [self.emit_prio_change(tid, args['priority'], ts)])
                return  # meta event
            case "thread_end":
                self.end_ongoing_events(tid, ts)
            case str(n) if "lock_" in n:
                lock_name = self.get_lock_name(msg)
                event_name += "(" + lock_name + ")"

        if tid == KERNEL_TID:
            self.emit_kernel_cpu_if_new(cpu)
            track_uuid = self.kernel_cpu_uids[cpu]

        if not track_uuid:
            track_uuid = self.tid_to_events_track_uid[tid]

        packet = perfetto_trace_pb2.TracePacket()
        packet.timestamp = ts + (1 if shift_by_ns else 0)
        packet.track_event.type = phase
        packet.track_event.name = event_name
        packet.track_event.track_uuid = track_uuid
        packet.trusted_packet_sequence_id = PACKET_SEQ

        if flow_id:
            packet.track_event.flow_ids.append(flow_id)

        if phase == TrackEvent.Type.TYPE_SLICE_BEGIN:
            self.add_ongoing_event(tid, packet)

        skip = False

        if phase == TrackEvent.Type.TYPE_SLICE_END:
            if not self.pop_ongoing_event(tid, packet):
                # Not all synthetic END events will have a corresponding BEGIN,
                # e.g. threads may be scheduled (thread_scheduling) without
                # being woken up (thread_waking). In such case, don't emit END
                # event as not to confuse parsers
                skip = True

        if not skip:
            self.print_trace_packets([packet])

        if update_cpu:
            self.update_cpu_virtual_thread(msg, cpu)

    def emit_events(self, msg):
        for begin in self.synthetic_begin.get(msg.event.name, []):
            self.emit_event(msg, begin, TrackEvent.Type.TYPE_SLICE_BEGIN)
            return

        for end in self.synthetic_end.get(msg.event.name, []):
            self.emit_event(msg, end, TrackEvent.Type.TYPE_SLICE_END)
            return

        self.emit_event(msg, msg.event.name, TrackEvent.Type.TYPE_INSTANT)

    def convert(self, path, output_path):
        eprint("converting CTF to perfetto")

        start = time.time()

        self.dest = open(output_path, "wb")

        msg_it = bt2.TraceCollectionMessageIterator(path)

        self.last_flush = time.time()

        for msg in msg_it:
            if hasattr(msg, "event"):
                self.emit_events(msg)

        self.flush_current_trace()

        self.dest.close()

        stop = time.time()
        eprint(f"finished in {stop - start:.2f} s")

        if self.warn_unknown_threads:
            eprint(
                "WARN: there were threads missing metadata - they will be marked as UNKNOWN")


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(
            "usage: " + sys.argv[0] + " [ctf path] [output path]\n")
        sys.exit(1)

    ctf_path = sys.argv[1]
    output_path = sys.argv[2]

    e = Emitter()

    e.convert(ctf_path, output_path)


if __name__ == "__main__":
    main()
