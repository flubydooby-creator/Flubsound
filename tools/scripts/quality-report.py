#!/usr/bin/env python3
"""Sound-quality report against tests/quality_targets.json (docs/11 E59).

Runs `flubsound-cli quality --json` once for every (setting, latency profile,
hygiene sample rate) the selected rows need, reads each row's metric from the
`quality` object and checks it with the rules tests/test_cli_quality.cpp uses:

  * a row without `recorded` must meet its target (op <= or >=; `byProfile`
    overrides the target per profile)
  * a row with `recorded` is a KNOWN_GAP: it passes when it meets its target
    (then drop `recorded`), or when it is not worse than the value recorded
    for this build's compiler by more than `margin`; without a value for that
    compiler the reference (gcc) value applies with twice the margin

The compiler is read from `flubsound-cli version` ("built with: gcc ...").
tests/test_cli_quality.cpp evaluates the per-PR rows on every build; this
script evaluates every row (the nightly profile x rate matrix) and is the
tool for tuning sessions:

    python3 tools/scripts/quality-report.py --cli build/tools/flubsound-cli/flubsound-cli
    python3 tools/scripts/quality-report.py --cli ... --tier per-pr --only E05
    python3 tools/scripts/quality-report.py --cli ... --json report.json

`--update-recorded` writes the measured values of the selected known-gap rows
into `recorded` for this compiler (re-recording the ratchet after a change
that moves a gap on purpose; the change names it). Rows whose target is met
are reported, not rewritten.

Exit code 0 when every row passes, 1 when any fails, 2 on usage / render
errors. Standard library only.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_TARGETS = ROOT / "tests" / "quality_targets.json"
TARGETS_FORMAT = "flubsound-quality-targets"


def compiler_of(cli):
    out = subprocess.run([cli, "version"], capture_output=True, text=True, check=True).stdout
    m = re.search(r"built with:\s*(\w+)", out)
    return m.group(1) if m else "gcc"


def expand_rows(targets, compiler):
    rows = []
    for r in targets["rows"]:
        for profile in r.get("profiles") or [""]:
            for rate in r.get("rates") or [48000]:
                target = r.get("byProfile", {}).get(profile, r["target"])
                margin = r.get("margin", 0.5)
                recorded = r.get("recorded", {})
                value = recorded.get(compiler)
                if value is None and "gcc" in recorded:
                    value, margin = recorded["gcc"], 2.0 * margin
                rows.append({
                    "id": r["id"], "item": r.get("item", ""), "setting": r["setting"], "profile": profile,
                    "rate": int(rate), "metric": r["metric"], "op": r["op"], "target": target,
                    "recorded": value, "margin": margin, "tier": r["tier"], "source": r.get("source", ""),
                })
    return rows


def metric_at(root, path):
    v = root
    for part in path.split("."):
        if isinstance(v, list):
            i = int(part)
            if i >= len(v):
                return None
            v = v[i]
        elif isinstance(v, dict):
            v = v.get(part)
        else:
            return None
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def evaluate(row, value):
    lower_is_better = row["op"] == "<="
    met = value <= row["target"] if lower_is_better else value >= row["target"]
    if met:
        return "met" if row["recorded"] is None else "gap met"
    if row["recorded"] is not None:
        limit = row["recorded"] + row["margin"] if lower_is_better else row["recorded"] - row["margin"]
        ok = value <= limit if lower_is_better else value >= limit
        return "known gap" if ok else "REGRESSED"
    return "FAIL"


def dump_targets(targets):
    """The targets file's layout: one line per row, so a re-record is a one-line diff per row."""
    lines = ["{"]
    keys = list(targets.keys())
    for i, k in enumerate(keys):
        comma = "," if i + 1 < len(keys) else ""
        if k == "rows":
            lines.append('  "rows": [')
            for j, r in enumerate(targets["rows"]):
                lines.append("    " + json.dumps(r) + ("," if j + 1 < len(targets["rows"]) else ""))
            lines.append("  ]" + comma)
        elif k == "settings":
            lines.append('  "settings": {')
            names = list(targets["settings"].keys())
            for j, name in enumerate(names):
                lines.append(f"    {json.dumps(name)}: {json.dumps(targets['settings'][name])}" + ("," if j + 1 < len(names) else ""))
            lines.append("  }" + comma)
        else:
            text = json.dumps(targets[k], indent=2).replace("\n", "\n  ")
            lines.append(f"  {json.dumps(k)}: {text}{comma}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def run_quality(cli, args):
    proc = subprocess.run([cli, "quality", *args, "--json", "--quiet"], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"flubsound-cli quality {' '.join(args)} failed: {proc.stderr.strip()}")
    return json.loads(proc.stdout)["quality"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True, help="path to the flubsound-cli executable")
    ap.add_argument("--targets", default=str(DEFAULT_TARGETS), help="targets file (default: %(default)s)")
    ap.add_argument("--tier", choices=["per-pr", "nightly", "all"], default="all", help="rows to evaluate (default: all)")
    ap.add_argument("--only", nargs="*", help="only rows whose id contains one of these strings")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel quality runs (default: CPU count)")
    ap.add_argument("--json", metavar="FILE", help="also write the results as JSON")
    ap.add_argument("--update-recorded", action="store_true", help="write measured known-gap values for this compiler")
    a = ap.parse_args()

    try:
        targets = json.loads(pathlib.Path(a.targets).read_text(encoding="utf-8"))
        if targets.get("format") != TARGETS_FORMAT:
            raise ValueError(f"not a {TARGETS_FORMAT} file")
        compiler = compiler_of(a.cli)
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    rows = [r for r in expand_rows(targets, compiler)
            if (a.tier == "all" or r["tier"] == a.tier) and (not a.only or any(s in r["id"] for s in a.only))]
    if not rows:
        print("error: no rows selected", file=sys.stderr)
        return 2

    # One quality run per (setting, profile, rate); the non-hygiene families
    # are pinned at 48 kHz, so their rows share the 48 kHz run.
    def key(r):
        return (r["setting"], r["profile"], r["rate"] if r["metric"].startswith("hygiene.") else 48000)

    runs = {}
    for r in rows:
        k = key(r)
        if k not in runs:
            setting, profile, rate = k
            args = list(targets["settings"][setting]["args"]) + (["--profile", profile] if profile else []) + ["--rate", str(rate)]
            runs[k] = args
    print(f"compiler {compiler}: {len(rows)} rows, {len(runs)} quality runs ...", file=sys.stderr)
    results = {}
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
            futures = {pool.submit(run_quality, a.cli, args): k for k, args in runs.items()}
            for f in concurrent.futures.as_completed(futures):
                results[futures[f]] = f.result()
    except (RuntimeError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    report = []
    for r in rows:
        value = metric_at(results[key(r)], r["metric"])
        status = "NO VALUE" if value is None else evaluate(r, value)
        report.append({**r, "value": value, "status": status})

    header = f"{'status':<10} {'id':<28} {'profile':<11} {'rate':>6}  {'value':>9}  {'target':>12}  {'recorded':>10}"
    print(header)
    for e in sorted(report, key=lambda e: (e["status"] in ("met", "known gap", "gap met"), e["id"], e["profile"], e["rate"])):
        value = "-" if e["value"] is None else f"{e['value']:.2f}"
        recorded = "" if e["recorded"] is None else f"{e['recorded']:.2f}+-{e['margin']:.1f}"
        print(f"{e['status']:<10} {e['id']:<28} {e['profile'] or '-':<11} {e['rate']:>6}  {value:>9}  {e['op']} {e['target']:>9.2f}  {recorded:>10}")
    failed = [e for e in report if e["status"] in ("FAIL", "REGRESSED", "NO VALUE")]
    print(f"{len(report) - len(failed)} of {len(report)} rows pass"
          + (f"; {sum(e['status'] == 'gap met' for e in report)} known gap(s) now meet their target (drop `recorded`)"
             if any(e["status"] == "gap met" for e in report) else ""))

    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"compiler": compiler, "rows": report}, indent=2) + "\n", encoding="utf-8")

    if a.update_recorded:
        measured = {}
        for e in report:
            if e["recorded"] is not None and e["value"] is not None and e["status"] != "gap met":
                measured.setdefault(e["id"], []).append(e["value"])
        changed = 0
        for r in targets["rows"]:
            if r["id"] in measured and "recorded" in r:
                values = measured[r["id"]]
                worst = max(values) if r["op"] == "<=" else min(values)
                r["recorded"][compiler] = round(worst, 2)
                changed += 1
        pathlib.Path(a.targets).write_text(dump_targets(targets), encoding="utf-8")
        print(f"recorded {changed} known-gap row(s) for {compiler} in {a.targets}", file=sys.stderr)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
