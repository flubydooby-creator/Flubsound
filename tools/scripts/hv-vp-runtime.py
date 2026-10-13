#!/usr/bin/env python3
"""Samples Hyper-V's per-vCPU run time for the PipeWire xrun case (docs/11 E48).

A Linux guest on Hyper-V (GitHub's Azure runners) has no steal clock:
/proc/stat reports no steal, and time the hypervisor takes from a vCPU while
a thread runs on it is counted as that thread's CPU time. Hyper-V's
synthetic MSR HV_X64_MSR_VP_RUNTIME (0x40000010) counts, per virtual
processor, the time it really ran (100 ns units, including the hypervisor's
work on its behalf). Sampling it next to the data thread's CPU time bounds
the hypervisor's share: a cycle charged C ns of CPU time while its vCPU ran
V ns had at least C - V ns taken by the hypervisor
(tests/app/CallbackStallAttribution.h).

Needs root and the msr module (/dev/cpu/N/msr; each read of another CPU's
MSR is an interrupt on that CPU). Run it at idle priority:

    sudo modprobe msr
    sudo chrt -i 0 python3 tools/scripts/hv-vp-runtime.py OUT.bin [PERIOD_MS]

OUT.bin (world-readable): a header of little-endian int32 numCpus and the
CPU ids, then fixed records of two int64 CLOCK_MONOTONIC ns stamps (before
and after the MSR reads: at idle priority it can be preempted in between,
so a reader bracketing a span takes the record before it by its second
stamp and the one after it by its first) and one uint64 VP run time
(100 ns units) per CPU. It runs until killed. A first read that fails
prints why and exits 2.
"""

import os
import struct
import sys
import time

HV_X64_MSR_VP_RUNTIME = 0x40000010


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out_path = sys.argv[1]
    period = float(sys.argv[2]) / 1000.0 if len(sys.argv) > 2 else 0.001
    cpus = sorted(int(name) for name in os.listdir("/dev/cpu") if name.isdigit()) if os.path.isdir("/dev/cpu") else []
    if not cpus:
        print("hv-vp-runtime: no /dev/cpu/N (modprobe msr)", file=sys.stderr)
        return 2
    fds = []
    for cpu in cpus:
        try:
            fd = os.open(f"/dev/cpu/{cpu}/msr", os.O_RDONLY)
            os.pread(fd, 8, HV_X64_MSR_VP_RUNTIME)
            fds.append(fd)
        except OSError as error:
            print(f"hv-vp-runtime: CPU {cpu}: cannot read HV_X64_MSR_VP_RUNTIME: {error}", file=sys.stderr)
            return 2
    record = struct.Struct("<qq" + "Q" * len(fds))
    old = os.umask(0o022)
    out = os.open(out_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    os.umask(old)
    os.write(out, struct.pack("<i" + "i" * len(cpus), len(cpus), *cpus))
    print(f"hv-vp-runtime: {len(cpus)} CPUs, a sample every {period * 1000:.2f} ms into {out_path}", flush=True)
    while True:
        start = time.monotonic_ns()
        values = [struct.unpack("<Q", os.pread(fd, 8, HV_X64_MSR_VP_RUNTIME))[0] for fd in fds]
        done = time.monotonic_ns()
        os.write(out, record.pack(start, done, *values))
        time.sleep(period)


if __name__ == "__main__":
    sys.exit(main())
