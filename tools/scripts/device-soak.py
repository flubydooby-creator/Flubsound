#!/usr/bin/env python3
"""Real-device soak matrix of the desktop app (docs/11 E53, R1.5).

Runs `Flubsound Pro --device-soak` (app/Source/shell/DeviceSoak.h) once per
row, one after the other (rows share the device, so they never overlap),
and prints one table of what each found: callback timing (late and
over-budget callbacks, the longest callback against its period), xruns,
device restarts and errors, the discontinuity detector's findings on the
device-bound output by class, and memory growth. Every row writes its own
JSON report and summary into --out.

    python tools/scripts/device-soak.py --app "build/app/FlubsoundPro_artefacts/Release/Flubsound Pro.exe" \
        --device "Digital Audio (S/PDIF) (High Definition Audio Device)" --minutes 10 --out soak-reports
    python tools/scripts/device-soak.py --app ... --device ... --rows default --minutes 30 --out soak-reports

The rows (--rows picks them by name, comma separated; default: the first three):
  low-default   "Windows Audio (Low Latency Mode)", the device's default buffer, Balanced
  low-min       the same at the smallest buffer the device offers
  shared        "Windows Audio" (shared mode) at its default buffer, Balanced
  default       the app's first-run configuration (as low-default), for long runs

Use an output nothing is connected to (or nobody listens to): the soak
plays generated music and a game scene at -6 dB with automation, which
reaches full scale (the 7.1 fold over 0 dBFS, strip gain up to +6 dB,
protection cycled through Off). The app refuses the system default output
(exit 3, nothing opened; it has no --allow-audible here) and an output the
device type does not list. The output is pinned: the app only ever asks for
that output, and a device JUCE opens by itself instead (its fallback when the
pinned one fails) plays silence and is closed, which ends the row (exit 4).
The app is a windowed program, so its output is only kept when redirected,
as this script does (each row's .log).

Exit code 0 when every row ran clean, 1 when a row had findings, 2 when a
row could not run (bad arguments, device missing, aborted).
Standard library only.
"""
import argparse
import json
import os
import subprocess
import sys
import time

ROWS = {
    "low-default": ["--type", "Windows Audio (Low Latency Mode)", "--profile", "balanced"],
    "low-min": ["--type", "Windows Audio (Low Latency Mode)", "--buffer", "min", "--profile", "balanced"],
    "shared": ["--type", "Windows Audio", "--profile", "balanced"],
    "default": ["--type", "Windows Audio (Low Latency Mode)", "--profile", "balanced"],
}


def run_row(app, device, name, extra, minutes, seed, out_dir, interval):
    report = os.path.join(out_dir, f"{name}.json")
    cmd = [app, "--device-soak", "--device", device, "--minutes", str(minutes), "--seed", str(seed),
           "--report", report] + extra
    if interval:
        cmd += ["--interval", str(interval)]
    started = time.time()
    with open(os.path.join(out_dir, f"{name}.log"), "w", encoding="utf-8") as log:
        code = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT)
    data = None
    if os.path.exists(report):
        with open(report, encoding="utf-8") as f:
            data = json.load(f)
    return code, data, time.time() - started


def row_line(name, code, data):
    if data is None:
        return f"{name:12s} exit {code}: no report (see {name}.log)"
    t = data["timing"]
    d = data["discontinuities"]
    dev = data["device"]
    m = data["memory"]
    classes = d["perClass"]
    return (f"{name:12s} exit {code} {data['verdict']:8s} {dev['bufferSize']:5d} smp {data['secondsAnalysed']:7.1f} s  "
            f"late {t['late']:3d} over {t['overBudget']:3d} max {t['durationMaxMs']:6.2f} ms ({100 * t['maxLoad']:5.1f} %)  "
            f"xruns {t['xrunsDevice']:3d}  restarts {dev['restarts']} errors {dev['errors']}  "
            f"clicks {d['click']} dropouts {d['dropout']} nan {d['non-finite']} dc {d['dc-step']} "
            f"[trans {classes['transition']} static {classes['static']} prog {classes['programme']} restart {classes['restart']} "
            f"gap {classes.get('gap', 0)} headroom {classes.get('headroom', 0)}]  "
            f"mem {m.get('privateGrowthAfterFirstMinuteMB', 0):+.2f} MB")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--app", required=True, help="the Flubsound Pro executable")
    ap.add_argument("--device", required=True, help="the output, exactly as the device type lists it")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--rows", default="low-default,low-min,shared")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--interval", type=float, default=0.0, help="mean ms between actions (default: the app's 2000)")
    ap.add_argument("--out", default="device-soak-reports")
    args = ap.parse_args()

    names = [r.strip() for r in args.rows.split(",") if r.strip()]
    unknown = [n for n in names if n not in ROWS]
    if unknown:
        print(f"unknown rows: {', '.join(unknown)} (known: {', '.join(ROWS)})", file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)

    worst = 0
    lines = []
    for i, name in enumerate(names):
        print(f"[{time.strftime('%H:%M:%S')}] {name}: {args.minutes} min ...", flush=True)
        code, data, wall = run_row(args.app, args.device, name, ROWS[name], args.minutes, args.seed + i, args.out, args.interval)
        line = row_line(name, code, data)
        print(f"  {line}  (wall {wall:.0f} s)", flush=True)
        lines.append(line)
        worst = max(worst, 2 if code >= 2 else code)

    print("\nsummary")
    for line in lines:
        print("  " + line)
    return worst


if __name__ == "__main__":
    sys.exit(main())
