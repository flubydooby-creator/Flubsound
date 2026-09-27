#!/usr/bin/env python3
"""Scripted all-preset render diff (docs/11 E59 slice).

Renders every factory preset (presets/factory/*.json) on a fixed set of pinned
test programmes with `flubsound-cli process --bands --json`, reduces each
render to a small set of numbers and compares them with a stored baseline:

  * output loudness: integrated LUFS, LRA, true peak, RMS (the CLI's report)
  * octave-band levels of the output, 31.5 Hz .. 16 kHz (`--bands`)
  * the level of the output mid in consecutive 1 s windows (time profile:
    pumping, level drift, the Night Mode post-event hole)
  * render.stats: limiter / glue / compressor GR, clip energy, THD+N, bass
    protection, footsteps band, governor scale, AutoLevel / AutoDrive
  * the compensated latency

It is the before/after evidence docs/11 E59 asks to attach to every
preset-, macro- or mode-band-affecting change until E52's golden renders
gate CI. Typical use:

    # on the base commit (or use the committed baseline, see below)
    python3 tools/scripts/preset-render-diff.py --cli build/tools/flubsound-cli/flubsound-cli \\
        --baseline /tmp/before.json --update
    # on the change
    python3 tools/scripts/preset-render-diff.py --cli build/tools/flubsound-cli/flubsound-cli \\
        --baseline /tmp/before.json

Exit code 0 when every value is within tolerance, 1 when anything moved
(a table of the changes is printed, largest first), 2 on usage / render
errors. `--update` writes the baseline instead of comparing.

The committed baseline, tests/golden/preset-render-baseline.json, was recorded
with a gcc Release build on Linux x86-64. Other compilers and CPUs can differ
by a few hundredths of a dB (FMA contraction, libm); feedback loops (governor,
AutoLevel) can amplify that. Compare like with like: record a baseline on the
base commit with the same build when the committed one does not match.

Standard library only; the programmes are generated here, deterministically
(xorshift32 noise, fixed seeds), so the script needs no audio files.
"""
import argparse
import array
import concurrent.futures
import json
import math
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PRESET_DIR = ROOT / "presets" / "factory"
DEFAULT_BASELINE = ROOT / "tests" / "golden" / "preset-render-baseline.json"
FS = 48000
BASELINE_FORMAT = "flubsound-preset-render-baseline"

# ---- deterministic signal generation ---------------------------------------


class XorShift32:
    """Same sequence as flub::FastRandom (computed in double here): uniform in [-1, 1)."""

    def __init__(self, seed):
        self.state = seed or 1

    def bipolar(self):
        s = self.state
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.state = s
        return (s >> 8) * (2.0 / 16777216.0) - 1.0


def pink(n, rms_level, seed):
    """Paul Kellet's refined pink filter on white noise, scaled to rms_level."""
    rng = XorShift32(seed)
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0
    out = [0.0] * n
    acc = 0.0
    for i in range(n):
        w = rng.bipolar()
        b0 = 0.99886 * b0 + w * 0.0555179
        b1 = 0.99332 * b1 + w * 0.0750759
        b2 = 0.96900 * b2 + w * 0.1538520
        b3 = 0.86650 * b3 + w * 0.3104856
        b4 = 0.55000 * b4 + w * 0.5329522
        b5 = -0.7616 * b5 - w * 0.0168980
        p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362
        b6 = w * 0.115926
        out[i] = p
        acc += p * p
    g = rms_level / math.sqrt(acc / n) if acc > 0 else 0.0
    return [v * g for v in out]


def band_pass(x, f0, q):
    """RBJ band-pass, 0 dB peak."""
    w0 = 2.0 * math.pi * f0 / FS
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    b0, a1, a2 = alpha / a0, -2.0 * math.cos(w0) / a0, (1.0 - alpha) / a0
    x1 = x2 = y1 = y2 = 0.0
    y = [0.0] * len(x)
    for i, v in enumerate(x):
        o = b0 * (v - x2) - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def kick(t_in_beat, peak):
    """55 Hz kick, tau 100 ms, 350 ms long."""
    if t_in_beat >= 0.35:
        return 0.0
    return peak * math.exp(-t_in_beat / 0.1) * math.sin(2.0 * math.pi * 55.0 * t_in_beat)


def programme_music(seconds=6.0):
    """Drum-like programme (the tests' makeProgramme): chirp kicks every 500 ms,
    noise hats, a 55 Hz bass line and a 440 / 660 Hz pad; about -17 LUFS."""
    n = int(seconds * FS)
    rng = XorShift32(1234)
    left, right = [0.0] * n, [0.0] * n
    level = 0.25
    for i in range(n):
        t = i / FS
        beat = math.fmod(t, 0.5)
        k = math.exp(-beat * 18.0) * math.sin(2.0 * math.pi * (50.0 + 80.0 * math.exp(-beat * 30.0)) * beat)
        hat = (0.3 if math.fmod(t + 0.25, 0.5) < 0.03 else 0.0) * rng.bipolar()
        bass = 0.4 * math.sin(2.0 * math.pi * 55.0 * t)
        pad = 0.15 * math.sin(2.0 * math.pi * 440.0 * t) + 0.1 * math.sin(2.0 * math.pi * 660.0 * t + 0.3)
        left[i] = level * (k + hat + bass + pad)
        right[i] = level * (k + 0.8 * hat + bass + 0.7 * pad)
    return [left, right]


def programme_game_quiet(seconds=6.0):
    """Quiet game ambience: -50 dBFS-RMS pink bed with a centred 40 ms footstep
    burst (3.2 kHz band noise, Hann, -56 dBFS RMS over its length) every
    400 ms from 0.5 s on (docs/11 E19's quiet-material case)."""
    n = int(seconds * FS)
    bed = pink(n, 10 ** (-50 / 20), 777)
    rng = XorShift32(4242)
    band = band_pass([rng.bipolar() for _ in range(n)], 3200.0, 1.0)
    band_rms = math.sqrt(sum(v * v for v in band) / n)
    length = int(0.040 * FS)
    g = 10 ** (-56 / 20) / band_rms / math.sqrt(3.0 / 8.0)
    x = bed[:]
    onset = int(0.5 * FS)
    while onset + length < n:
        for i in range(length):
            w = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / length)
            x[onset + i] += g * w * band[onset + i]
        onset += int(0.4 * FS)
    return [x, x[:]]


def programme_tone_kicks(seconds=4.0):
    """2 kHz at -20 dBFS peak under 55 Hz kicks (-6 dBFS peak) every 500 ms:
    the pumping stimulus of tests/test_known_gaps.cpp."""
    n = int(seconds * FS)
    x = [0.0] * n
    for i in range(n):
        t = i / FS
        beat = math.fmod(t + 0.25, 0.5)
        x[i] = 0.1 * math.sin(2.0 * math.pi * 2000.0 * t) + (kick(beat, 0.5) if t >= 0.25 else 0.0)
    return [x, x[:]]


def programme_ambush(seconds=10.0):
    """Ambush scene: -50 dBFS-RMS pink ambience, 1 s of automatic fire from 4 s
    (10 shots/s, white noise, tau 15 ms, -12 dBFS peak), then ambience."""
    n = int(seconds * FS)
    x = pink(n, 10 ** (-50 / 20), 1357)
    rng = XorShift32(2468)
    for shot in range(10):
        onset = int((4.0 + 0.1 * shot) * FS)
        for i in range(int(0.1 * FS)):
            x[onset + i] += 0.25 * math.exp(-i / (0.015 * FS)) * rng.bipolar()
    return [x, x[:]]


def programme_surround(seconds=4.0):
    """7.1 (FL FR FC LFE BL BR SL SR): independent -30 dBFS-RMS pink on the six
    main channels and a 50 Hz tone at -12 dBFS peak on the LFE (the 7.1 fold /
    virtualiser path, docs/11 E01)."""
    n = int(seconds * FS)
    chans = []
    for c in range(8):
        if c == 3:
            chans.append([0.25 * math.sin(2.0 * math.pi * 50.0 * i / FS) for i in range(n)])
        else:
            chans.append(pink(n, 10 ** (-30 / 20), 9000 + c))
    return chans


# name -> (generator, keep the 1 s level profile). The stationary programmes
# keep no profile: their integrated level says the same.
PROGRAMMES = {
    "music": (programme_music, True),
    "game-quiet": (programme_game_quiet, True),
    "tone-kicks": (programme_tone_kicks, False),
    "ambush": (programme_ambush, True),
    "surround-7.1": (programme_surround, False),
}

# ---- WAV I/O (float32) -------------------------------------------------------

FLOAT_GUID = bytes.fromhex("0300000000001000800000aa00389b71")


def write_wav(path, channels):
    n = len(channels[0])
    nch = len(channels)
    inter = array.array("f", [0.0] * (n * nch))
    for c, ch in enumerate(channels):
        inter[c::nch] = array.array("f", ch)
    if sys.byteorder != "little":
        inter.byteswap()
    data = inter.tobytes()
    if nch <= 2:
        fmt = struct.pack("<HHIIHH", 3, nch, FS, FS * 4 * nch, 4 * nch, 32)
    else:
        mask = 0x63F if nch == 8 else 0x3F
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, nch, FS, FS * 4 * nch, 4 * nch, 32, 22, 32, mask) + FLOAT_GUID
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def read_wav_float(path):
    """Reads the CLI's float32 output: returns planar channels."""
    raw = pathlib.Path(path).read_bytes()
    pos, nch, data = 12, 0, None
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, nch = struct.unpack("<HH", body[:4])
            bits = struct.unpack("<H", body[14:16])[0]
            if bits != 32 or tag not in (3, 0xFFFE):
                raise RuntimeError(f"{path}: expected float32 output")
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if data is None or nch == 0:
        raise RuntimeError(f"{path}: no audio data")
    samples = array.array("f")
    samples.frombytes(data[: len(data) // 4 * 4])
    if sys.byteorder != "little":
        samples.byteswap()
    return [samples[c::nch] for c in range(nch)]


def window_levels(channels, seconds=1.0):
    """dBFS RMS of the mid signal in consecutive windows (a partial last window
    is dropped)."""
    left, right = channels[0], channels[1]
    length = int(seconds * FS)
    levels = []
    for start in range(0, len(left) - length + 1, length):
        acc = 0.0
        for a, b in zip(left[start:start + length], right[start:start + length]):
            m = 0.5 * (a + b)
            acc += m * m
        levels.append(round(10.0 * math.log10(acc / length), 2) if acc > 0 else None)
    return levels


def pumping(channels):
    """tone-kicks only: the 2 kHz tone's level in 20 ms windows over 1..4 s
    (RBJ band-pass, Q 4, left channel). Returns (p95 - p5, median - min) in dB;
    the input tone is steady, so any spread is gain modulation by the kicks."""
    y = band_pass(channels[0][int(0.5 * FS):], 2000.0, 4.0)[int(0.5 * FS):]  # settle 0.5 s, keep 1..4 s
    length = int(0.020 * FS)
    levels = []
    for start in range(0, len(y) - length + 1, length):
        acc = sum(v * v for v in y[start:start + length])
        levels.append(10.0 * math.log10(max(acc / length, 1e-30)))
    levels.sort()
    pick = lambda p: levels[int(round(p * (len(levels) - 1)))]
    return round(pick(0.95) - pick(0.05), 2), round(pick(0.5) - levels[0], 2)


# ---- rendering and reduction --------------------------------------------------


def stat_values(stats):
    """The render.stats values kept in the baseline (flat, 2 decimals)."""
    def get(*keys):
        v = stats
        for k in keys:
            v = v.get(k) if isinstance(v, dict) else None
        return v

    band4 = next((b for b in stats.get("modeBands", []) if b.get("band") == 4), {})
    return {
        "limiterGrMaxDb": get("limiter", "grMaxDb"),
        "limiterGrMeanDb": get("limiter", "grMeanDb"),
        "limiterOver1DbPercent": get("limiter", "over1DbPercent"),
        "glueGrMaxDb": get("glue", "grMaxDb"),
        "clipEnergyMaxDb": get("clipper", "energyMaxDb"),
        "thdnMaxDb": get("distortion", "thdnMaxDb"),
        "compGrMaxDb": get("compressor", "grMaxDb"),
        "compUpwardMaxDb": get("compressor", "upwardMaxDb"),
        "bassProtectionMaxDb": get("bassProtectionMaxDb"),
        "band4MaxDb": band4.get("maxDb"),
        "band4MeanDb": band4.get("meanDb"),
        "governorScaleMin": get("governor", "scaleMin"),
        "autoLevelMinDb": get("leveller", "autoLevelMinDb"),
        "autoLevelMaxDb": get("leveller", "autoLevelMaxDb"),
        "autoDriveMaxDb": get("leveller", "autoDriveMaxDb"),
    }


def render_one(cli, preset_file, programme, stimulus, work):
    out = work / f"{preset_file.stem}__{programme}.wav"
    cmd = [str(cli), "process", "-i", str(stimulus), "-o", str(out), "-p", str(preset_file), "--bands", "--json", "-q"]
    # Presets no longer set the latency profile (docs/11 E40): render each in
    # the one it suggests, as it is meant to be used (Balanced otherwise).
    suggested = json.loads(preset_file.read_text(encoding="utf-8")).get("suggestedLatencyProfile")
    if suggested:
        cmd += ["--profile", suggested.lower().replace(" ", "-")]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({proc.returncode}): {proc.stderr.strip()}")
    doc = json.loads(proc.stdout)
    o = doc["output"]
    values = {
        "latencySamples": doc["render"]["latencySamples"],
        "integratedLufs": o.get("integratedLufs"),
        "loudnessRangeLu": o.get("loudnessRangeLu"),
        "truePeakDbtp": o.get("truePeakDbtp"),
        "rmsDbfs": o.get("rmsDbfs"),
    }
    for band in doc.get("outputBands", []):
        values[f"band{band['hz']:g}Hz"] = band["db"]
    if PROGRAMMES[programme][1] or programme == "tone-kicks":
        audio = read_wav_float(out)
        if PROGRAMMES[programme][1]:
            for i, level in enumerate(window_levels(audio)):
                values[f"window{i}s"] = level
        if programme == "tone-kicks":
            values["pumpingP95P5Db"], values["pumpingDipDb"] = pumping(audio)
    values.update(stat_values(doc["render"].get("stats", {})))
    out.unlink()
    return values


def tolerance_for(key, db_tol):
    if key == "latencySamples":
        return 0
    if key.endswith("Percent"):
        return 1.0
    if key == "governorScaleMin":
        return 0.005
    return db_tol


def compare(baseline, current, db_tol):
    """Rows (|delta| sort key, preset, programme, key, before, after) beyond tolerance."""
    rows = []
    for name in sorted(set(baseline) | set(current)):
        before, after = baseline.get(name), current.get(name)
        preset, _, programme = name.partition("|")
        if before is None or after is None:
            rows.append((math.inf, preset, programme, "(render)", "missing" if before is None else "present",
                         "missing" if after is None else "present"))
            continue
        for key in sorted(set(before) | set(after)):
            b, a = before.get(key), after.get(key)
            if b is None and a is None:
                continue
            if b is None or a is None:
                rows.append((math.inf, preset, programme, key, b, a))
                continue
            delta = a - b
            if abs(delta) > tolerance_for(key, db_tol) + 1e-9:
                rows.append((abs(delta), preset, programme, key, b, a))
    rows.sort(key=lambda r: (-r[0], r[1], r[2], r[3]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cli", required=True, help="path to the flubsound-cli executable")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE), help="baseline JSON (default: %(default)s)")
    ap.add_argument("--update", action="store_true", help="write the baseline instead of comparing")
    ap.add_argument("--presets", nargs="*", help="only these preset files (stems, e.g. gaming-night-mode)")
    ap.add_argument("--programmes", nargs="*", choices=sorted(PROGRAMMES), help="only these programmes")
    ap.add_argument("--tolerance-db", type=float, default=0.1, help="dB / LU tolerance (default %(default)s)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel renders (default: CPU count)")
    ap.add_argument("--keep", metavar="DIR", help="keep the generated programmes in DIR")
    args = ap.parse_args()

    cli = pathlib.Path(args.cli)
    if not cli.is_file():
        print(f"error: {cli} is not a file", file=sys.stderr)
        return 2
    presets = sorted(PRESET_DIR.glob("*.json"))
    if args.presets:
        presets = [p for p in presets if p.stem in set(args.presets)]
        missing = set(args.presets) - {p.stem for p in presets}
        if missing:
            print(f"error: unknown presets: {', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    programmes = args.programmes or list(PROGRAMMES)

    work = pathlib.Path(args.keep) if args.keep else pathlib.Path(tempfile.mkdtemp(prefix="flub-preset-diff-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        stimuli = {}
        for name in programmes:
            path = work / f"programme-{name}.wav"
            if not path.exists():
                write_wav(path, PROGRAMMES[name][0]())
            stimuli[name] = path
        print(f"rendering {len(presets)} presets x {len(programmes)} programmes ...", file=sys.stderr)
        current = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {pool.submit(render_one, cli, p, name, stimuli[name], work): f"{p.stem}|{name}"
                       for p in presets for name in programmes}
            for future in concurrent.futures.as_completed(futures):
                current[futures[future]] = future.result()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    baseline_path = pathlib.Path(args.baseline)
    if args.update:
        # Columnar: one key list per programme, one value list per render
        # (one render per line, so a diff of the baseline itself is readable).
        keys = {}
        for name in sorted(current):
            keys.setdefault(name.partition("|")[2], list(current[name]))
        lines = [f'"{name}":' + json.dumps([current[name].get(k) for k in keys[name.partition("|")[2]]], separators=(",", ":"))
                 for name in sorted(current)]
        head = json.dumps({"format": BASELINE_FORMAT, "version": 1, "sampleRate": FS,
                           "note": "tools/scripts/preset-render-diff.py --update: per programme the value names, "
                                   "per 'preset|programme' render the values in that order",
                           "keys": {k: keys[k] for k in sorted(keys)}}, separators=(",", ":"))
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(head[:-1] + ',"renders":{\n' + ",\n".join(lines) + "\n}}\n", encoding="utf-8")
        print(f"wrote {baseline_path} ({len(current)} renders)", file=sys.stderr)
        return 0

    if not baseline_path.is_file():
        print(f"error: no baseline at {baseline_path} (record one with --update)", file=sys.stderr)
        return 2
    doc = json.loads(baseline_path.read_text(encoding="utf-8"))
    if doc.get("format") != BASELINE_FORMAT:
        print(f"error: {baseline_path} is not a preset-render baseline", file=sys.stderr)
        return 2
    baseline = {name: dict(zip(doc["keys"][name.partition("|")[2]], values)) for name, values in doc["renders"].items()}
    if args.presets or args.programmes:
        baseline = {k: v for k, v in baseline.items() if k in current}
    rows = compare(baseline, current, args.tolerance_db)
    if not rows:
        print(f"no change: {len(current)} renders within tolerance ({args.tolerance_db} dB) of {baseline_path}")
        return 0
    print(f"{len(rows)} value(s) moved beyond tolerance ({args.tolerance_db} dB) against {baseline_path}:")
    print(f"{'preset':34} {'programme':13} {'value':24} {'before':>9} {'after':>9} {'delta':>8}")

    def fmt(v):
        return f"{v:9.2f}" if isinstance(v, (int, float)) else f"{str(v):>9}"

    for d, preset, programme, key, b, a in rows:
        delta = f"{a - b:+8.2f}" if isinstance(a, (int, float)) and isinstance(b, (int, float)) else f"{'':>8}"
        print(f"{preset:34} {programme:13} {key:24} {fmt(b)} {fmt(a)} {delta}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
