#!/usr/bin/env python3
"""Train, evaluate and export Flubsound's neural voice cleanup model (numpy only).

    python tools/neural/train_voice_cleanup.py all --work <scratch dir>
    python tools/neural/train_voice_cleanup.py all --work <dir> --quick      # a smoke run

Steps (each can be run alone; they share the --work folder):
  generate  synthetic training / validation clips -> features and targets (cached)
  train     the network (BPTT, Adam), keeping the best validation state
  export    presets/neural/voice-cleanup.fnn (int8), the embedded C++ copy,
            the C++ tests' reference data and the quality-floor test clip
  evaluate  the held-out test set: SNR / segmental SNR gain, clean-speech band
            error, noise attenuation in pauses, VAD accuracy, per noise type;
            with --cli <flubsound-cli> also the spectral noise gate (R2.7) and
            the C++ model rendered by the CLI, on the same files
Everything is seeded (clip k of a set uses seed base + k), so a run is
reproducible on the same numpy version. Data never leaves the work folder
except the files `export` writes into the repository.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import time

# Tiny matrices: one BLAS thread per process is fastest (and the workers run in parallel).
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")

import numpy as np  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import fvdsp  # noqa: E402
import fvsynth  # noqa: E402
import tinynet as tn  # noqa: E402

MODEL_VERSION = 1
MODEL_NAME = "Flubsound voice cleanup v1 (experimental)"
MODEL_FILE = ROOT / "presets" / "neural" / "voice-cleanup.fnn"
MODEL_CARD = ROOT / "presets" / "neural" / "voice-cleanup.model.json"
EMBED_CPP = ROOT / "core" / "src" / "neural" / "VoiceCleanupModelData.cpp"
TEST_HEADER = ROOT / "tests" / "neural_reference_data.h"
TEST_CLIP = ROOT / "tests" / "data" / "neural" / "voice-cleanup-clip.wav"

# Post-processing the C++ runner applies to the network's gains (VoiceCleanupRunner.cpp):
GAIN_FLOOR = 0.03          # -30 dB: never a hard gate (keeps a trace of the room, less musical noise)
RELEASE = 0.775            # g_t >= RELEASE * g_{t-1}: RNNoise's 0.6 per 10 ms, at 5 ms frames

SEEDS = {"train": 100000, "val": 200000, "test": 300000}
TEST_NOISES = ["white", "pink", "brown", "fan", "keyboard", "babble"]
TEST_SNRS = [-5.0, 0.0, 5.0, 10.0, 15.0, 20.0]


def arch():
    F = fvdsp.NUM_FEATURES
    return [
        {"kind": tn.LAYER_CONV1D, "inputs": [0], "out": 48, "act": tn.ACT_TANH, "kernel": 3},   # 1
        {"kind": tn.LAYER_GRU, "inputs": [1], "out": 48},                                       # 2
        {"kind": tn.LAYER_DENSE, "inputs": [2], "out": 1, "act": tn.ACT_SIGMOID},               # 3 VAD
        {"kind": tn.LAYER_GRU, "inputs": [1, 2], "out": 64},                                    # 4
        {"kind": tn.LAYER_DENSE, "inputs": [2, 4], "out": fvdsp.NBANDS, "act": tn.ACT_SIGMOID},  # 5 gains
    ], [5, 3], F


# ---- data -----------------------------------------------------------------------
def clip_features(seed, seconds, noise_types=None, snr_db=None):
    s, n, meta = fvsynth.make_clip(seed, seconds, noise_types, snr_db)
    x = s + n
    spec_x = fvdsp.stft_frames(x)
    spec_s = fvdsp.stft_frames(s)
    spec_n = fvdsp.stft_frames(n)
    feats = fvdsp.features_from_spec(spec_x, x).astype(np.float32)
    g, mask = fvdsp.wiener_targets(spec_s, spec_n)
    vad = fvdsp.vad_targets(spec_s)
    return feats, g.astype(np.float32), mask, vad.astype(np.float32), meta


def _gen_one(args):
    seed, seconds = args
    f, g, m, v, meta = clip_features(seed, seconds)
    return f, g, m, v, meta


def generate(work, n_train, n_val, seconds, procs):
    from multiprocessing import Pool
    out = {}
    for split, count in (("train", n_train), ("val", n_val)):
        jobs = [(SEEDS[split] + k, seconds) for k in range(count)]
        t0 = time.time()
        with Pool(procs) as pool:
            res = pool.map(_gen_one, jobs, chunksize=2)
        for name, idx in (("feats", 0), ("gains", 1), ("masks", 2), ("vads", 3)):
            np.save(work / f"{split}-{name}.npy", np.stack([r[idx] for r in res]))
        out[split] = {"clips": count, "seconds": seconds, "hours": count * seconds / 3600.0,
                      "generate_s": time.time() - t0, "kinds": {}}
        for r in res:
            k = r[4]["kind"] if r[4]["kind"] != "mix" else r[4]["noise"]
            out[split]["kinds"][k] = out[split]["kinds"].get(k, 0) + 1
        print(f"[generate] {split}: {count} clips x {seconds:.0f} s in {time.time() - t0:.0f} s", flush=True)
    (work / "data.json").write_text(json.dumps(out, indent=1))
    return out


def load_split(work, split, mmap=True):
    mode = "r" if mmap else None
    return {name: np.load(work / f"{split}-{name}.npy", mmap_mode=mode) for name in ("feats", "gains", "masks", "vads")}


# ---- training -------------------------------------------------------------------
def losses(outs, g_true, mask, vad_true, need_grad=True):
    g_pred, v_pred = outs
    sp = np.sqrt(np.maximum(g_pred, 1e-12))
    d = sp - np.sqrt(g_true)
    m = mask.astype(g_pred.dtype)
    denom = max(1.0, float(m.sum()))
    gl = float(np.sum(m * (d * d + 10.0 * d ** 4)) / denom)
    vp = np.clip(v_pred[..., 0], 1e-7, 1 - 1e-7)
    vl = float(-np.mean(vad_true * np.log(vp) + (1 - vad_true) * np.log(1 - vp)))
    total = 10.0 * gl + 0.5 * vl
    if not need_grad:
        return total, gl, vl, None
    dg = 10.0 * m * (2 * d + 40.0 * d ** 3) / (2 * sp) / denom
    dv = (0.5 * (vp - vad_true) / (vp * (1 - vp)) / vad_true.size)[..., None]
    return total, gl, vl, [dg.astype(g_pred.dtype), dv.astype(v_pred.dtype)]


_WORKER = {}


def _worker_init(work, mu, sd, seed):
    _WORKER["data"] = load_split(pathlib.Path(work), "train")
    _WORKER["mu"], _WORKER["sd"] = mu.astype(np.float32), sd.astype(np.float32)
    specs, outputs, F = arch()
    _WORKER["net"] = tn.Net(F, specs, outputs, seed=seed)


def _worker_grad(args):
    state, ci, oi, seq = args
    net, d = _WORKER["net"], _WORKER["data"]
    net.load_state(state)
    idx = np.argsort(ci, kind="stable")                  # sorted reads from the memory map
    ci, oi = ci[idx], oi[idx]
    sl = (ci[:, None], oi[:, None] + np.arange(seq)[None, :])
    x = (np.asarray(d["feats"][sl]) - _WORKER["mu"]) / _WORKER["sd"]
    outs = net.forward(x)
    total, gl, vl, grads = losses(outs, np.asarray(d["gains"][sl]), np.asarray(d["masks"][sl]), np.asarray(d["vads"][sl]))
    return net.backward(grads), (total, gl, vl)


def validate(net, v, mu, sd):
    vt = vg = 0.0
    n = v["feats"].shape[0]
    for k in range(0, n, 16):
        x = (np.asarray(v["feats"][k:k + 16]) - mu) / sd
        o = net.forward(x.astype(np.float32))
        t_, g_, _, _ = losses(o, np.asarray(v["gains"][k:k + 16]), np.asarray(v["masks"][k:k + 16]),
                              np.asarray(v["vads"][k:k + 16]), need_grad=False)
        m = min(16, n - k)
        vt += t_ * m
        vg += g_ * m
    return vt / n, vg / n


def train(work, steps, batch, seq, lr, seed, procs):
    """Synchronous data-parallel BPTT: `procs` workers each take batch / procs sequences of the
    current weights, the parent averages their gradients and takes one Adam step."""
    from multiprocessing import Pool
    d = load_split(work, "train")
    v = load_split(work, "val", mmap=False)
    nclip, nfr, F = d["feats"].shape
    # feature statistics from a seeded sample of the training clips
    srng = np.random.default_rng(seed + 1)
    pick = np.sort(srng.choice(nclip, size=min(nclip, 300), replace=False))
    sample = np.asarray(d["feats"][pick]).reshape(-1, F).astype(np.float64)
    mu, sd = sample.mean(axis=0), sample.std(axis=0) + 1e-3
    specs, outputs, F = arch()
    net = tn.Net(F, specs, outputs, seed=seed)
    opt = tn.Adam(net, lr=lr)
    rng = np.random.default_rng(seed)
    workers = max(1, min(procs, batch))
    per = batch // workers
    best, best_state, hist = np.inf, None, []
    t0 = time.time()
    eval_every = max(25, steps // 40)
    with Pool(workers, initializer=_worker_init, initargs=(str(work), mu, sd, seed)) as pool:
        for step in range(1, steps + 1):
            ci = rng.integers(0, nclip, per * workers)
            oi = rng.integers(0, nfr - seq + 1, per * workers)
            state = net.state()
            res = pool.map(_worker_grad, [(state, ci[w * per:(w + 1) * per], oi[w * per:(w + 1) * per], seq)
                                          for w in range(workers)])
            pg = [{k: sum(r[0][li][k] for r in res) / workers for k in res[0][0][li]} for li in range(len(net.layers))]
            total = float(np.mean([r[1][0] for r in res]))
            gl = float(np.mean([r[1][1] for r in res]))
            vl = float(np.mean([r[1][2] for r in res]))
            frac = step / steps
            warm = min(1.0, step / 50.0)
            cur = lr * warm * (0.5 * (1 + np.cos(np.pi * frac)) * 0.95 + 0.05)   # warm-up, cosine to 5 %
            gnorm = opt.step(pg, cur)
            if step % eval_every == 0 or step == steps:
                vt, vg = validate(net, v, mu, sd)
                hist.append({"step": step, "train": total, "val": vt, "val_gain": vg, "lr": cur, "gnorm": gnorm,
                             "elapsed_s": time.time() - t0})
                print(f"[train] step {step}/{steps} train {total:.4f} (gain {gl:.4f} vad {vl:.3f}) val {vt:.4f} "
                      f"(gain {vg:.4f}) lr {cur:.2e} |g| {gnorm:.2f} {time.time() - t0:.0f} s", flush=True)
                if vt < best:
                    best, best_state = vt, net.state()
                    np.savez(work / "model.npz", mu=mu, sd=sd, **best_state)
    net.load_state(best_state)
    np.savez(work / "model.npz", mu=mu, sd=sd, **best_state)
    info = {"steps": steps, "batch": per * workers, "seq": seq, "lr": lr, "seed": seed, "workers": workers,
            "best_val": best, "train_s": time.time() - t0, "history": hist, "params": net.param_count(),
            "frames_seen": steps * per * workers * seq}
    (work / "train.json").write_text(json.dumps(info, indent=1))
    return info


def load_trained(work):
    specs, outputs, F = arch()
    net = tn.Net(F, specs, outputs, seed=0)
    z = np.load(work / "model.npz")
    net.load_state({k: z[k] for k in z.files if "." in k})
    return net, z["mu"], z["sd"]


def folded(net, mu, sd):
    """The normalisation folded into the first layer (a causal conv on the raw features)."""
    out = tn.Net(net.num_inputs, [], [], seed=0)
    out.sizes, out.layers, out.outputs = list(net.sizes), [], list(net.outputs)
    import copy
    for li, layer in enumerate(net.layers):
        lay = copy.deepcopy(layer)
        if li == 0:
            W = layer.p["W"].astype(np.float64).copy()
            b = layer.p["b"].astype(np.float64).copy()
            F = layer.in_dim
            for j in range(layer.kernel):
                blk = W[:, j * F:(j + 1) * F]
                b -= blk @ (mu / sd)
                W[:, j * F:(j + 1) * F] = blk / sd
            lay.p["W"], lay.p["b"] = W.astype(np.float32), b.astype(np.float32)
        out.layers.append(lay)
    return out


# ---- inference as the C++ runner does it --------------------------------------------
def run_model(state_net, x):
    """Gains (frames, 22) after post-processing, and the VAD (frames,), for signal x."""
    sn = tn.StreamingNet(state_net, tn.quantised_copy(state_net, int8=True))
    f = fvdsp.features(x).astype(np.float32)
    g = np.empty((len(f), fvdsp.NBANDS))
    v = np.empty(len(f))
    prev = np.ones(fvdsp.NBANDS)
    for t in range(len(f)):
        gg, vv = sn.step(f[t])
        gg = np.maximum(gg.astype(np.float64), GAIN_FLOOR)
        gg = np.maximum(gg, RELEASE * prev)
        prev = gg
        g[t], v[t] = gg, vv[0]
    return g, v


def run_model_batch(net_q, xs):
    """Fast batched version of run_model for many equal-length clips (sequence forward)."""
    feats = np.stack([fvdsp.features(x) for x in xs]).astype(np.float32)
    g_raw, v_raw = net_q.forward(feats)
    g = np.maximum(g_raw.astype(np.float64), GAIN_FLOOR)
    for t in range(1, g.shape[1]):
        g[:, t] = np.maximum(g[:, t], RELEASE * g[:, t - 1])
    return g, v_raw[..., 0]


# ---- metrics ----------------------------------------------------------------------
def snr_db(s, y):
    return 10 * np.log10(np.sum(s ** 2) / (np.sum((y - s) ** 2) + 1e-20) + 1e-20)


def seg_snr(s, y, active, frame=960):
    m = len(s) // frame
    S = s[:m * frame].reshape(m, frame)
    E = (y - s)[:m * frame].reshape(m, frame)
    act = active[:m * frame].reshape(m, frame).mean(axis=1) > 0.5
    v = 10 * np.log10(np.sum(S ** 2, axis=1) / (np.sum(E ** 2, axis=1) + 1e-20) + 1e-20)
    v = np.clip(v, -10.0, 35.0)
    return float(v[act].mean()) if act.any() else float("nan")


def band_error_db(s_in, s_out, vad):
    """Mean |band level change| of clean speech (active frames, bands within 40 dB of the frame's loudest)."""
    a = fvdsp.band_energies(fvdsp.stft_frames(s_in))
    b = fvdsp.band_energies(fvdsp.stft_frames(s_out))
    n = min(len(a), len(b), len(vad))
    a, b, act = a[:n], b[:n], vad[:n] > 0.5
    sel = (a > a.max(axis=1, keepdims=True) * 1e-4) & act[:, None] & (a > 1e-9)
    if not sel.any():
        return float("nan")
    return float(np.mean(np.abs(10 * np.log10((b[sel] + 1e-20) / (a[sel] + 1e-20)))))


def pause_attenuation_db(x, y, vad_samples):
    sel = vad_samples < 0.5
    if sel.sum() < 4800:
        return float("nan")
    return float(10 * np.log10((np.sum(y[sel] ** 2) + 1e-20) / (np.sum(x[sel] ** 2) + 1e-20)))


def test_set(work, seconds):
    clips = []
    k = 0
    for noise in TEST_NOISES:
        for snr in TEST_SNRS:
            for rep in range(3):
                clips.append((SEEDS["test"] + k, [noise], snr))
                k += 1
    for rep in range(6):
        clips.append((SEEDS["test"] + k, ["pink"], 40.0))   # (nearly) clean speech
        k += 1
    return clips


def _test_clip(args):
    seed, noise, snr, seconds = args
    s, n, meta = fvsynth.make_clip(seed, seconds, noise, snr, clean_prob=0.0, noise_only_prob=0.0)
    return s, n, meta


def evaluate(work, seconds, procs, cli=None):
    from multiprocessing import Pool
    net, mu, sd = load_trained(work)
    import copy
    fnet = folded(net, mu, sd)
    net_q = copy.deepcopy(fnet)          # the weights the C++ runtime holds (int8 dequantised), in float64
    for li, k, v in net_q.params():
        net_q.layers[li].p[k] = v.astype(np.float64)
    net_q.load_state({k: v.astype(np.float64) for k, v in tn.quantised_copy(fnet, int8=True).items()})
    clips = test_set(work, seconds)
    with Pool(procs) as pool:
        data = pool.map(_test_clip, [(c[0], c[1], c[2], seconds) for c in clips], chunksize=2)
    xs = [s + n for s, n, _ in data]
    ss = [s for s, _, _ in data]
    t0 = time.time()
    g_mix, v_mix = run_model_batch(net_q, xs)
    g_cln, _ = run_model_batch(net_q, ss)
    infer_s = time.time() - t0
    rows = []
    wav_dir = work / "eval-wav"
    wav_dir.mkdir(exist_ok=True)
    (wav_dir / "mix").mkdir(exist_ok=True)
    (wav_dir / "clean").mkdir(exist_ok=True)
    for i, ((s, n, meta), x) in enumerate(zip(data, xs)):
        vad_f = fvdsp.vad_targets(fvdsp.stft_frames(s))
        vad_s = np.repeat(vad_f, fvdsp.HOP)[:len(s)]
        y = fvdsp.render(x, g_mix[i])
        yc = fvdsp.render(s, g_cln[i])
        nf = min(len(vad_f), v_mix.shape[1])
        row = {"seed": meta["seed"], "noise": meta["noise"], "snr_in_target": meta["snr_db"],
               "snr_in": snr_db(s, x), "model": {}}
        row["model"] = {"snr_out": snr_db(s, y), "seg_in": seg_snr(s, x, vad_s), "seg_out": seg_snr(s, y, vad_s),
                        "band_err": band_error_db(s, yc, vad_f), "clean_level": float(10 * np.log10(np.sum(yc ** 2) / np.sum(s ** 2))),
                        "pause_att": pause_attenuation_db(x, y, vad_s),
                        "vad_acc": float(np.mean((v_mix[i, :nf] > 0.5) == (vad_f[:nf] > 0.5)))}
        rows.append(row)
        name = f"clip{i:03d}.wav"
        fvdsp.write_wav_float(wav_dir / "mix" / name, x)
        fvdsp.write_wav_float(wav_dir / "clean" / name, s)
    result = {"clips": len(rows), "seconds": seconds, "python_inference_s": infer_s, "rows": rows}
    if cli:
        result["cli"] = evaluate_cli(work, cli, data, rows)
    print_table(result)
    (work / "eval.json").write_text(json.dumps(result, indent=1))
    return result


# Every module off but the one under test (the chain's other stages are then a latency-compensated delay).
GATE_OFF = ["eq.on=off", "dyneq.on=off", "bass.on=off", "clarity.on=off", "sat.on=off", "smooth.amount=0",
            "spatial.on=off", "comp.on=off", "max.on=off", "virt.on=off", "boost=0", "autolevel.on=off", "contour.on=off"]


def evaluate_cli(work, cli, data, rows):
    """Renders the test files through the C++ chain: the spectral noise gate alone (Quality profile,
    default and 30 dB reduction) and the neural voice cleanup alone, then scores them like the model."""
    wav_dir = work / "eval-wav"
    configs = {
        "gate": ["--profile", "quality", "--set", "gate.on=on"],
        "gate30": ["--profile", "quality", "--set", "gate.on=on", "--set", "gate.reduction=30"],
        "neural_cpp": ["--profile", "balanced", "--neural", "voice-cleanup"],
    }
    base = []
    for kv in GATE_OFF:
        base += ["--set", kv]
    out = {}
    for name, extra in configs.items():
        for src in ("mix", "clean"):
            dst = wav_dir / f"{name}-{src}"
            dst.mkdir(exist_ok=True)
            cmd = [cli, "batch", str(wav_dir / src), str(dst), "--quiet"] + base + extra
            t0 = time.time()
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(" ".join(cmd))
                print(r.stdout[-2000:], r.stderr[-2000:])
                raise SystemExit(f"CLI {name} failed")
            print(f"[evaluate] CLI {name}/{src}: {time.time() - t0:.1f} s", flush=True)
        for i, ((s, n, meta), row) in enumerate(zip(data, rows)):
            x = s + n
            y = fvdsp.read_wav(wav_dir / f"{name}-mix" / f"clip{i:03d}.wav")[0][:, 0]
            yc = fvdsp.read_wav(wav_dir / f"{name}-clean" / f"clip{i:03d}.wav")[0][:, 0]
            vad_f = fvdsp.vad_targets(fvdsp.stft_frames(s))
            vad_s = np.repeat(vad_f, fvdsp.HOP)[:len(s)]
            row[name] = {"snr_out": snr_db(s, y), "seg_out": seg_snr(s, y, vad_s), "band_err": band_error_db(s, yc, vad_f),
                         "clean_level": float(10 * np.log10(np.sum(yc ** 2) / np.sum(s ** 2))),
                         "pause_att": pause_attenuation_db(x, y, vad_s)}
        out[name] = " ".join(extra)
    return out


def summarise(rows, key, pred=lambda r: True):
    sel = [r for r in rows if pred(r) and key in r]
    if not sel:
        return None
    def m(f):
        vals = [f(r) for r in sel]
        vals = [v for v in vals if np.isfinite(v)]
        return float(np.mean(vals)) if vals else float("nan")
    return {
        "n": len(sel),
        "dsnr": m(lambda r: r[key]["snr_out"] - r["snr_in"]),
        "dseg": m(lambda r: r[key]["seg_out"] - r["model"]["seg_in"]),
        "band_err": m(lambda r: r[key]["band_err"]),
        "clean_level": m(lambda r: r[key]["clean_level"]),
        "pause_att": m(lambda r: r[key]["pause_att"]),
        "vad_acc": m(lambda r: r[key].get("vad_acc", float("nan"))),
    }


def print_table(result):
    rows = result["rows"]
    methods = ["model"] + [k for k in ("neural_cpp", "gate", "gate30") if k in rows[0]]
    print("\n| set | method | n | dSNR dB | dSegSNR dB | clean band err dB | clean level dB | pause att dB | VAD acc |")
    print("|---|---|---|---|---|---|---|---|---|")
    sets = [("all noisy", lambda r: r["snr_in_target"] < 30)] + \
        [(nz, (lambda nz: lambda r: r["noise"] == nz and r["snr_in_target"] < 30)(nz)) for nz in TEST_NOISES] + \
        [(f"SNR {s:+.0f}", (lambda s: lambda r: r["snr_in_target"] == s)(s)) for s in TEST_SNRS] + \
        [("clean speech", lambda r: r["snr_in_target"] >= 30)]
    table = []
    for label, pred in sets:
        for mth in methods:
            s = summarise(rows, mth, pred)
            if s is None:
                continue
            table.append({"set": label, "method": mth, **s})
            print(f"| {label} | {mth} | {s['n']} | {s['dsnr']:+.2f} | {s['dseg']:+.2f} | {s['band_err']:.2f} | "
                  f"{s['clean_level']:+.2f} | {s['pause_att']:+.1f} | {s['vad_acc']:.3f} |")
    result["table"] = table


# ---- export -----------------------------------------------------------------------
def c_array(data, per_line=24):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + per_line]) + ",")
    return "\n".join(lines)


def c_float(v):
    s = f"{float(v):.9g}"
    if not any(ch in s for ch in ".en"):
        s += ".0"
    return s + "f"


def f_array(vals, per_line=8):
    vals = [float(v) for v in np.asarray(vals).reshape(-1)]
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append("    " + ", ".join(c_float(v) for v in vals[i:i + per_line]) + ",")
    return "\n".join(lines)


def embed_cpp(model_bytes):
    return (
        "// GENERATED by tools/neural/train_voice_cleanup.py (export) from\n"
        "// presets/neural/voice-cleanup.fnn - do not edit by hand.\n"
        "// tests/test_voice_cleanup.cpp checks it against the file.\n"
        "#include \"flub/neural/VoiceCleanupRunner.h\"\n\n"
        "namespace flub\n{\nnamespace\n{\n"
        f"const unsigned char kVoiceCleanupModel[{len(model_bytes)}] = {{\n{c_array(model_bytes)}\n}};\n"
        "} // namespace\n\n"
        "const unsigned char* voiceCleanupModelData() noexcept { return kVoiceCleanupModel; }\n"
        f"std::size_t voiceCleanupModelSize() noexcept {{ return {len(model_bytes)}; }}\n"
        "} // namespace flub\n"
    )


def reference_test_net(seed=7):
    """A small net with every layer type and activation, for the runtime's math test."""
    specs = [
        {"kind": tn.LAYER_CONV1D, "inputs": [0], "out": 6, "act": tn.ACT_TANH, "kernel": 3},
        {"kind": tn.LAYER_DENSE, "inputs": [0, 1], "out": 5, "act": tn.ACT_RELU},
        {"kind": tn.LAYER_GRU, "inputs": [2], "out": 7},
        {"kind": tn.LAYER_DENSE, "inputs": [3], "out": 4, "act": tn.ACT_LINEAR},
        {"kind": tn.LAYER_GRU, "inputs": [1, 3], "out": 3},
        {"kind": tn.LAYER_DENSE, "inputs": [5, 4], "out": 2, "act": tn.ACT_SIGMOID},
        {"kind": tn.LAYER_CONV1D, "inputs": [6], "out": 3, "act": tn.ACT_LINEAR, "kernel": 1},
    ]
    net = tn.Net(5, specs, [6, 4, 7], seed=seed)
    rng = np.random.default_rng(seed)
    for li, k, v in net.params():
        v += rng.normal(0.0, 0.3, v.shape).astype(v.dtype)   # non-zero biases too
    return net


def export(work):
    net, mu, sd = load_trained(work)
    fnet = folded(net, mu, sd)
    MODEL_FILE.parent.mkdir(parents=True, exist_ok=True)
    data = tn.write_model(fnet, MODEL_FILE, model_version=MODEL_VERSION, feature_set=fvdsp.FEATURE_SET,
                          sample_rate=fvdsp.SAMPLE_RATE, frame_size=fvdsp.HOP, name=MODEL_NAME, int8=True)
    EMBED_CPP.write_text(embed_cpp(data), encoding="utf-8", newline="\n")
    print(f"[export] {MODEL_FILE.relative_to(ROOT)}: {len(data)} bytes, {fnet.param_count()} parameters (int8)")

    # Reference data for the C++ tests.
    rnet = reference_test_net()
    lines = ["// GENERATED by tools/neural/train_voice_cleanup.py (export) - do not edit by hand.",
             "// Reference outputs computed with numpy (float64 arithmetic on float32 weights) for",
             "// tests/test_tinynet.cpp and tests/test_voice_cleanup.cpp.",
             "#pragma once", "", "#include <cstddef>", "", "namespace flub::neuralref", "{"]
    for tag, int8 in (("F32", False), ("Int8", True)):
        b = tn.write_model(rnet, work / f"ref-{tag}.fnn", model_version=3, feature_set=0, sample_rate=0, frame_size=1,
                           name=f"reference {tag}", int8=int8)
        st = tn.quantised_copy(rnet, int8=int8)
        sn = tn.StreamingNet(rnet, st)
        rng = np.random.default_rng(11)
        frames = 9
        xin = rng.normal(0.0, 1.0, (frames, rnet.num_inputs)).astype(np.float32)
        outs = []
        for t in range(frames):
            if t == 5:
                sn.reset()        # the C++ test resets at the same frame
            outs.append(np.concatenate(sn.step(xin[t])))
        lines.append(f"inline constexpr unsigned char kNet{tag}[{len(b)}] = {{\n{c_array(b)}\n}};")
        lines.append(f"inline constexpr int kNet{tag}Frames = {frames}, kNet{tag}ResetAt = 5;")
        lines.append(f"inline constexpr float kNet{tag}Input[{xin.size}] = {{\n{f_array(xin)}\n}};")
        lines.append(f"inline constexpr float kNet{tag}Output[{frames * 9}] = {{\n{f_array(np.stack(outs))}\n}};")
        lines.append("")
    lines.append("inline constexpr int kNetOutputSizes[3] = { 2, 4, 3 };")
    lines.append("")

    # Features and the shipped model's gains on a deterministic signal (0.25 s).
    n = 12000
    t = np.arange(n) / fvdsp.SAMPLE_RATE
    lcg = np.empty(n)
    state = 12345
    for i in range(n):   # a 32-bit LCG the C++ test reproduces exactly
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        lcg[i] = (state >> 8) / float(1 << 24) - 0.5
    sig = (0.3 * np.sin(2 * np.pi * 150.0 * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 3.0 * t))
           + 0.1 * np.sin(2 * np.pi * (300.0 * t + 4000.0 * t * t)) + 0.05 * lcg)
    sig32 = sig.astype(np.float32).astype(np.float64)
    feats = fvdsp.features(sig32)
    g, v = run_model(fnet, sig32)
    lines.append(f"inline constexpr int kSignalSamples = {n}, kSignalFrames = {len(feats)};")
    lines.append("// signal = 0.3 sin(2 pi 150 t)(0.5 + 0.5 sin(2 pi 3 t)) + 0.1 sin(2 pi (300 t + 4000 t^2)) + 0.05 lcg")
    lines.append(f"inline constexpr float kSignalFeatures[{feats.size}] = {{\n{f_array(feats)}\n}};")
    lines.append(f"inline constexpr float kSignalGains[{g.size}] = {{\n{f_array(g)}\n}};")
    lines.append(f"inline constexpr float kSignalVad[{v.size}] = {{\n{f_array(v)}\n}};")
    lines.append(f"inline constexpr float kGainFloor = {c_float(GAIN_FLOOR)}, kRelease = {c_float(RELEASE)};")
    lines.append("} // namespace flub::neuralref")
    TEST_HEADER.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"[export] {TEST_HEADER.relative_to(ROOT)}")

    # The quality-floor clip: held-out speech + fan noise at 5 dB SNR, 2 s (channel 0 speech, 1 noise).
    s, nz, meta = fvsynth.make_clip(SEEDS["test"] + 9999, 2.0, ["fan"], 5.0, clean_prob=0.0, noise_only_prob=0.0)
    TEST_CLIP.parent.mkdir(parents=True, exist_ok=True)
    fvdsp.write_wav(TEST_CLIP, np.stack([s, nz], axis=1))
    s16, _ = fvdsp.read_wav(TEST_CLIP)
    x = s16[:, 0] + s16[:, 1]
    gq, _ = run_model(fnet, x)
    y = fvdsp.render(x, gq)
    print(f"[export] {TEST_CLIP.relative_to(ROOT)}: {meta}, SNR {snr_db(s16[:, 0], x):.2f} -> {snr_db(s16[:, 0], y):.2f} dB")
    return {"bytes": len(data), "params": fnet.param_count(), "clip": meta,
            "clip_snr_in": snr_db(s16[:, 0], x), "clip_snr_out": snr_db(s16[:, 0], y)}


def write_card(work):
    info = json.loads((work / "train.json").read_text())
    data = json.loads((work / "data.json").read_text())
    ev = json.loads((work / "eval.json").read_text()) if (work / "eval.json").exists() else {}
    if ev and "table" not in ev:
        print_table(ev)
    exp = json.loads((work / "export.json").read_text()) if (work / "export.json").exists() else {}
    card = {
        "name": MODEL_NAME, "file": "voice-cleanup.fnn", "modelVersion": MODEL_VERSION,
        "status": "experimental",
        "what": "Per-band gains (22 bands) and a voice-activity output for speech in noise, RNNoise-style, "
                "trained from scratch on synthetic data only (tools/neural/).",
        "features": "flub-voice-v1: 48 kHz, 5 ms hop, 10 ms Vorbis window, 512-point FFT, 22 band log-energies + voicing",
        "architecture": [{k: v for k, v in s.items()} for s in arch()[0]],
        "parameters": info["params"], "weights": "int8, one float scale per row; biases float32",
        "postProcessing": {"gainFloor": GAIN_FLOOR, "releasePerFrame": RELEASE},
        "training": {k: info.get(k) for k in ("steps", "batch", "seq", "lr", "seed", "workers", "frames_seen", "best_val", "train_s")},
        "data": data, "export": exp,
        "evaluation": {"clips": ev.get("clips"), "seconds": ev.get("seconds"), "table": ev.get("table"),
                       "cli": ev.get("cli")},
        "numpy": np.__version__,
    }
    MODEL_CARD.write_text(json.dumps(card, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"[card] {MODEL_CARD.relative_to(ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("step", choices=["generate", "train", "export", "evaluate", "card", "all"])
    ap.add_argument("--work", required=True, help="scratch folder for data, checkpoints and evaluation files")
    ap.add_argument("--quick", action="store_true", help="tiny data and few steps (a smoke run)")
    ap.add_argument("--train-clips", type=int, default=1500)
    ap.add_argument("--val-clips", type=int, default=96)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--steps", type=int, default=2500)
    ap.add_argument("--batch", type=int, default=192)
    ap.add_argument("--seq", type=int, default=400)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--procs", type=int, default=max(1, min(12, (os.cpu_count() or 2) - 2)))
    ap.add_argument("--test-seconds", type=float, default=8.0)
    ap.add_argument("--cli", help="flubsound-cli executable for the C++ comparison in `evaluate`")
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    if a.quick:
        a.train_clips, a.val_clips, a.steps, a.batch, a.seq = 48, 8, 40, 24, 200
    timings = {}
    t0 = time.time()
    if a.step in ("generate", "all"):
        generate(work, a.train_clips, a.val_clips, a.seconds, a.procs)
        timings["generate_s"] = time.time() - t0
    if a.step in ("train", "all"):
        t1 = time.time()
        train(work, a.steps, a.batch, a.seq, a.lr, a.seed, a.procs)
        timings["train_s"] = time.time() - t1
    if a.step in ("export", "all"):
        (work / "export.json").write_text(json.dumps(export(work), indent=1))
    if a.step in ("evaluate", "all"):
        evaluate(work, 2.0 if a.quick else a.test_seconds, a.procs, a.cli)
    if a.step in ("card", "all"):
        write_card(work)
    print("timings", timings)


if __name__ == "__main__":
    main()
