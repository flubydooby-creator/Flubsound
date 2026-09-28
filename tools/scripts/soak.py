#!/usr/bin/env python3
"""Long offline soak of the processing chain (docs/11 E53 step 4, Linux slice).

Runs `flubsound-cli soak --json` for each setting of a matrix, in parallel,
for the same number of minutes each, and summarises what the discontinuity
detector found on the chain's output: clicks, dropouts, NaN / Inf runs and
DC steps, grouped by the automation action that came before them. The CI
runs the 10 s version of this (tests/test_soak.cpp); this script is the
nightly / release soak (E53: 1 h nightly, 8 h per release):

    python3 tools/scripts/soak.py --cli build/tools/flubsound-cli/flubsound-cli --minutes 12
    python3 tools/scripts/soak.py --cli ... --minutes 480 --jobs 4 --json soak.json
    python3 tools/scripts/soak.py --cli ... --minutes 1 --only fuzz

The matrix (--only picks rows whose name contains the text):
  user-music    defaults, Music, user automation (Boost, macros, parameters,
                modules, mode, factory presets, bypass, A/B)
  user-gaming   the same from Gaming mode
  user-fps      from Competitive FPS
  user-night    from Late Night, Low Latency profile, 128-sample blocks
  user-44k      defaults at 44.1 kHz, 256-sample blocks, protection normal
  fuzz          every non-structural parameter to a random value, 100 ms apart

Each row has its own seed (--seed adds to it), so a run is reproducible.
A detection whose last action is in --known (default "bypass -> on", the
open E53 finding: the dry path's true-peak limiter starts cold when the
global bypass engages) is reported as known and does not fail the run.
Detections made while the global bypass was engaged (the output is the
bypass reference, not the processing) are counted per row: in the fuzz row
the reference's true-peak limiter clicks when a large input.gain drives it
far over the ceiling (docs/11 E53).

Exit code 0 when no row found a discontinuity outside --known (and no
programme self-check failed), 1 otherwise, 2 on usage / run errors.
Standard library only.
"""
import argparse
import collections
import concurrent.futures
import json
import shlex
import subprocess
import sys
import time

MATRIX = [
    ("user-music", 1, []),
    ("user-gaming", 2, ["--mode", "gaming"]),
    ("user-fps", 3, ["--preset", "Competitive FPS"]),
    ("user-night", 4, ["--preset", "Late Night", "--profile", "low-latency", "--block", "128"]),
    ("user-44k", 5, ["--rate", "44100", "--block", "256", "--protection", "normal"]),
    ("fuzz", 6, ["--automation", "all", "--interval", "100"]),
]


def action_kind(text):
    """'eq.0.gain = -3.2 dB' -> 'eq.0.gain', 'bypass -> on' -> 'bypass -> on'."""
    if not text:
        return "(no action yet)"
    if text == "bypass = on":  # the fuzz sets the bypass like any parameter
        return "bypass -> on"
    if " -> " in text and text.split(" -> ")[0] in ("bypass", "mode"):
        return text
    return text.split(" = ")[0].split(" -> ")[0].split(" (")[0]


def run_row(cli, name, seed, extra, minutes):
    cmd = [cli, "soak", "--minutes", str(minutes), "--seed", str(seed), "--json", "--quiet"] + extra
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode not in (0, 1) or not p.stdout.strip():
        raise RuntimeError(f"{name}: {' '.join(cmd)} exited {p.returncode}: {p.stderr.strip()}")
    report = json.loads(p.stdout)
    report["name"] = name
    report["command"] = cmd
    report["wall"] = time.time() - t0
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cli", required=True, help="path to flubsound-cli")
    ap.add_argument("--minutes", type=float, default=10.0, help="minutes of programme per row (default 10)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel rows (default: all)")
    ap.add_argument("--seed", type=int, default=0, help="added to every row's seed")
    ap.add_argument("--only", default="", help="rows whose name contains this")
    ap.add_argument("--known", action="append", default=None,
                    help='last actions whose detections are known findings (default: "bypass -> on"); repeatable')
    ap.add_argument("--json", default="", help="write the full reports here")
    args = ap.parse_args()
    known = set(args.known if args.known is not None else ["bypass -> on"])

    rows = [r for r in MATRIX if args.only in r[0]]
    if not rows:
        print(f"no matrix row matches --only {args.only!r}", file=sys.stderr)
        return 2
    jobs = args.jobs if args.jobs > 0 else len(rows)
    reports = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(run_row, args.cli, name, seed + args.seed, extra, args.minutes) for name, seed, extra in rows]
            reports = [f.result() for f in futures]
    except (RuntimeError, OSError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    failed = False
    for r in reports:
        out, inp = r["output"], r["input"]
        by_kind = collections.Counter(action_kind(d["lastAction"]) for d in r["detections"])
        unknown = sum(n for kind, n in by_kind.items() if kind not in known)
        listed = sum(by_kind.values())
        unknown += max(0, out["total"] - listed)  # beyond the detector's list: count as unknown
        bypassed = sum(1 for d in r["detections"] if d.get("bypassed") and action_kind(d["lastAction"]) not in known)
        bad = unknown > 0 or inp["total"] > 0
        failed |= bad
        t = r["timing"]
        print(f"{r['name']:<12} {r['seconds'] / 60:6.1f} min  {r['actions']['total']:6d} actions  "
              f"click {out['click']:4d}  dropout {out['dropout']:3d}  non-finite {out['non-finite']:3d}  dc-step {out['dc-step']:3d}  "
              f"(set aside: {out['setAside']['kinks']} kinks, {out['setAside']['recurring']} recurring)  "
              f"not known {unknown} ({bypassed} while bypassed)  peak {r['outputPeakDbfs']} dBFS  {t['realtimeFactor']}x RT, max block {t['maxBlockMs']} ms  "
              f"{'FAIL' if bad else 'ok'}")
        if inp["total"] > 0:
            print(f"    programme self-check failed: {inp}")
        for kind, n in by_kind.most_common():
            first = next(d for d in r["detections"] if action_kind(d["lastAction"]) == kind)
            print(f"    {n:4d} after {kind}{' (known)' if kind in known else ''}: first at {first['seconds']} s, ch {first['channel']}, "
                  f"{first['type']} {first['levelDb']} dB, {first['lastActionAgeMs']} ms after '{first['lastAction']}'")
        print(f"    rerun: {shlex.join(r['command'])}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"format": "flubsound-soak-report", "minutes": args.minutes, "known": sorted(known), "rows": reports}, f, indent=1)
    print("soak: " + ("DISCONTINUITIES FOUND" if failed else "no discontinuities outside the known findings"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
