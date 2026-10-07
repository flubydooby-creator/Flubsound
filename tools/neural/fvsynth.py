"""Synthetic speech and noise for the voice cleanup model (numpy only, seeded).

Speech: a source-filter synthesiser. Phrases of syllables ((C)(C)V(C)) are
painted onto a 2.5 ms control grid: formant targets for vowels (Peterson &
Barney / Hillenbrand averages, scaled per speaker), nasals and approximants,
fricative noise bands (s, sh, f, th, z, v, h), plosives (closure, burst,
aspiration). Formant tracks are smoothed over 30 ms (coarticulation); the
fundamental follows a phrase declination with accents, question rises,
wander, jitter and shimmer. A glottal impulse train (voiced) and white noise
(fricatives, aspiration, breathiness) are filtered in the STFT domain by the
per-frame spectral envelopes (a parallel formant model times a source tilt),
each envelope normalised so the output RMS follows the amplitude tracks.
Speakers: male / female / child F0 and formant scales, tilt, breathiness,
jitter, shimmer and speaking rate.

Noise: white, pink, brown, fan (broadband + mains hum with harmonics + blade
tone), keyboard typing (key down / up clicks in bursts) and babble (3-8
voices from the speech synthesiser), with slow level wander; mixtures of two.
"""
import numpy as np

from fvdsp import SAMPLE_RATE as FS

CTRL = 120                      # control grid hop (2.5 ms)
STFT_FRAME, STFT_HOP = 1024, 256

# Adult male formants F1..F3 (Hz).
VOWELS = {
    "i": (270, 2290, 3010), "I": (390, 1990, 2550), "e": (530, 1840, 2480), "ae": (660, 1720, 2410),
    "a": (730, 1090, 2440), "o": (570, 840, 2410), "U": (440, 1020, 2240), "u": (300, 870, 2240),
    "V": (640, 1190, 2390), "er": (490, 1350, 1690), "E": (580, 1800, 2500), "oU": (500, 900, 2400),
}
VOWEL_KEYS = list(VOWELS)
DIPHTHONGS = [("a", "I"), ("e", "I"), ("o", "U"), ("a", "U"), ("o", "I")]

# Consonants: class, formants (or None = coarticulated), voiced dB, noise dB, noise (centre, width) or "formant"
CONSONANTS = {
    "m": ("nasal", (250, 1000, 2300), -8, None, None),
    "n": ("nasal", (250, 1600, 2500), -8, None, None),
    "ng": ("nasal", (250, 2000, 2600), -9, None, None),
    "l": ("approx", (350, 1000, 2600), -4, None, None),
    "r": ("approx", (420, 1150, 1550), -4, None, None),
    "w": ("approx", (300, 700, 2300), -5, None, None),
    "j": ("approx", (280, 2200, 3000), -5, None, None),
    "s": ("fric", None, None, -9, (6200, 4500)),
    "sh": ("fric", None, None, -7, (3100, 2600)),
    "f": ("fric", None, None, -21, (6000, 12000)),
    "th": ("fric", None, None, -23, (5500, 11000)),
    "z": ("vfric", None, -13, -15, (6000, 4500)),
    "v": ("vfric", None, -11, -25, (5000, 10000)),
    "h": ("h", None, None, -17, "formant"),
    "p": ("stop", None, None, -13, (900, 2500)),
    "t": ("stop", None, None, -10, (4200, 4000)),
    "k": ("stop", None, None, -11, (2200, 1800)),
    "b": ("vstop", None, None, -16, (900, 2500)),
    "d": ("vstop", None, None, -13, (4000, 4000)),
    "g": ("vstop", None, None, -14, (2100, 1800)),
}
ONSETS = ["p", "t", "k", "b", "d", "g", "m", "n", "s", "sh", "f", "th", "h", "v", "z", "l", "r", "w", "j"]
ONSET_W = np.array([6, 9, 6, 5, 6, 3, 6, 6, 8, 3, 4, 3, 5, 3, 2, 5, 5, 4, 2], dtype=float)
CODAS = ["m", "n", "ng", "s", "t", "k", "l", "r", "z", "d", "p", "f"]
CODA_W = np.array([4, 7, 2, 7, 8, 4, 4, 4, 3, 4, 2, 2], dtype=float)


def _db(x):
    return -np.inf if x is None else float(x)


class Speaker:
    def __init__(self, rng):
        kind = rng.choice(["male", "female", "child"], p=[0.45, 0.45, 0.10])
        self.kind = kind
        if kind == "male":
            self.f0 = rng.uniform(85, 150)
            self.scale = rng.uniform(0.93, 1.05)
        elif kind == "female":
            self.f0 = rng.uniform(160, 250)
            self.scale = rng.uniform(1.10, 1.22)
        else:
            self.f0 = rng.uniform(230, 320)
            self.scale = rng.uniform(1.25, 1.40)
        self.tilt = rng.uniform(6.0, 13.0)          # source slope, dB / octave
        self.breath = rng.uniform(-36.0, -14.0)     # aspiration under voicing, dB
        self.jitter = rng.uniform(0.003, 0.015)
        self.shimmer = rng.uniform(0.02, 0.08)
        self.rate = rng.uniform(0.8, 1.25)          # > 1 faster
        self.f4 = rng.uniform(3200, 3700) * self.scale
        self.f5 = rng.uniform(4100, 4700) * self.scale


class Track:
    """Control-grid parameter tracks painted phoneme by phoneme."""

    def __init__(self, frames):
        self.n = frames
        nan = np.full(frames, np.nan)
        self.F = [nan.copy() for _ in range(5)]
        self.B = [nan.copy() for _ in range(5)]
        self.A = [nan.copy() for _ in range(5)]
        self.vdb = np.full(frames, -np.inf)
        self.ndb = np.full(frames, -np.inf)
        self.nc = np.zeros(frames)       # noise centre (0 = formant-shaped)
        self.nw = np.ones(frames)        # noise width
        self.stress = np.zeros(frames)   # accent weight for the F0 contour
        self.phrase = np.full(frames, -1, dtype=np.int64)
        self.phrase_pos = np.zeros(frames)


def _paint(tr, a, b, **kw):
    a, b = max(0, a), min(tr.n, b)
    if b <= a:
        return
    for key, val in kw.items():
        if key in ("F", "B", "A"):
            arr = getattr(tr, key)
            for i, v in enumerate(val):
                if v is not None:
                    arr[i][a:b] = v
        else:
            getattr(tr, key)[a:b] = val


def _vowel_formants(spk, key, rng):
    f = VOWELS[key]
    jit = rng.uniform(0.95, 1.05, 3)
    return (f[0] * spk.scale * jit[0], f[1] * spk.scale * jit[1], f[2] * spk.scale * jit[2], spk.f4, spk.f5)


def build_utterance(rng, spk, frames, continuous=False):
    """Paints phrases until the track is full; returns the Track."""
    tr = Track(frames)
    ms = lambda v: max(1, int(round(v / spk.rate * FS / 1000.0 / CTRL)))
    pos = 0 if continuous else int(rng.uniform(0.0, 0.5) * FS / CTRL)
    phrase_id = 0
    while pos < frames:
        n_syl = int(rng.integers(2, 11))
        start = pos
        syl_spans = []
        question = rng.random() < 0.2
        for s in range(n_syl):
            word_start = s == 0 or rng.random() < 0.45
            if word_start and s > 0 and rng.random() < 0.35:
                pos += ms(rng.uniform(10, 70))
            stressed = (word_start and rng.random() < 0.6) or rng.random() < 0.15
            syl_a = pos
            # onset
            if rng.random() < 0.75:
                onset = [rng.choice(ONSETS, p=ONSET_W / ONSET_W.sum())]
                if rng.random() < 0.1:
                    onset = (["s", rng.choice(["p", "t", "k"])] if rng.random() < 0.5
                             else [rng.choice(["p", "b", "k", "g", "f"]), rng.choice(["l", "r"])])
                for c in onset:
                    pos = _paint_consonant(tr, rng, spk, c, pos, ms, onset=True)
            # nucleus
            v1 = rng.choice(VOWEL_KEYS)
            diph = rng.random() < 0.2
            dur = ms(rng.uniform(110, 260) if stressed else rng.uniform(60, 140))
            if s == n_syl - 1:
                dur = int(dur * 1.3)
            amp = rng.uniform(1.0, 4.0) if stressed else rng.uniform(-4.0, -1.0)
            bw = (rng.uniform(55, 90), rng.uniform(70, 120), rng.uniform(110, 170), 200.0, 260.0)
            amps = (1.0, 0.75, 0.45, 0.28, 0.18)
            if diph:
                d1, d2 = DIPHTHONGS[int(rng.integers(len(DIPHTHONGS)))]
                f1, f2 = _vowel_formants(spk, d1, rng), _vowel_formants(spk, d2, rng)
                half = dur // 2
                _paint(tr, pos, pos + half, F=f1, B=bw, A=amps, vdb=amp, ndb=amp + spk.breath, nc=0.0)
                _paint(tr, pos + half, pos + dur, F=f2, B=bw, A=amps, vdb=amp, ndb=amp + spk.breath, nc=0.0)
            else:
                f = _vowel_formants(spk, v1, rng)
                _paint(tr, pos, pos + dur, F=f, B=bw, A=amps, vdb=amp, ndb=amp + spk.breath, nc=0.0)
            if stressed:
                _paint(tr, pos, pos + dur, stress=rng.uniform(0.06, 0.25))
            pos += dur
            # coda
            if rng.random() < 0.4:
                pos = _paint_consonant(tr, rng, spk, rng.choice(CODAS, p=CODA_W / CODA_W.sum()), pos, ms, onset=False)
            syl_spans.append((syl_a, pos))
            if pos >= frames:
                break
        end = min(pos, frames)
        if end > start:
            tr.phrase[start:end] = phrase_id
            tr.phrase_pos[start:end] = np.linspace(0.0, 1.0, end - start)
            if question:
                tail = max(start, end - ms(250))
                tr.stress[tail:end] = np.maximum(tr.stress[tail:end], np.linspace(0.0, 0.35, end - tail))
        phrase_id += 1
        pos += ms(rng.uniform(40, 120)) if continuous else int(rng.uniform(0.15, 0.9) * FS / CTRL)
    return tr


def _paint_consonant(tr, rng, spk, c, pos, ms, onset):
    kind, form, vdb, ndb, noise = CONSONANTS[c]
    if kind in ("nasal", "approx"):
        dur = ms(rng.uniform(45, 90) if kind == "nasal" else rng.uniform(40, 80))
        f = tuple(v * spk.scale * rng.uniform(0.95, 1.05) for v in form) + (spk.f4, spk.f5)
        b = (rng.uniform(60, 100), 250.0 if kind == "nasal" else 110.0, 300.0 if kind == "nasal" else 160.0, 250.0, 300.0)
        a = (1.0, 0.25, 0.15, 0.08, 0.05) if kind == "nasal" else (1.0, 0.5, 0.3, 0.18, 0.1)
        _paint(tr, pos, pos + dur, F=f, B=b, A=a, vdb=vdb + rng.uniform(-2, 2), ndb=vdb + spk.breath, nc=0.0)
        return pos + dur
    if kind in ("fric", "vfric"):
        dur = ms(rng.uniform(80, 170) if c in ("s", "sh", "z") else rng.uniform(60, 120))
        centre, width = noise
        centre *= rng.uniform(0.9, 1.1) * (spk.scale ** 0.5)
        _paint(tr, pos, pos + dur, ndb=ndb + rng.uniform(-3, 3), nc=centre, nw=width,
               vdb=(vdb + rng.uniform(-2, 2)) if vdb is not None else -np.inf)
        return pos + dur
    if kind == "h":
        dur = ms(rng.uniform(40, 80))
        _paint(tr, pos, pos + dur, ndb=ndb + rng.uniform(-3, 3), nc=0.0)
        return pos + dur
    # stops: closure, burst, aspiration (unvoiced onsets)
    closure = ms(rng.uniform(35, 85))
    if kind == "vstop":
        _paint(tr, pos, pos + closure, vdb=-22.0, F=(200.0, 900.0, 2400.0, spk.f4, spk.f5),
               B=(80.0, 400.0, 500.0, 400.0, 400.0), A=(1.0, 0.05, 0.02, 0.01, 0.01))
    pos += closure
    if not onset and rng.random() < 0.5:
        return pos                       # unreleased coda stop
    burst = max(2, ms(rng.uniform(8, 16)))
    centre, width = noise
    _paint(tr, pos, pos + burst, ndb=ndb + rng.uniform(-3, 3), nc=centre * rng.uniform(0.85, 1.15), nw=width)
    pos += burst
    if kind == "stop" and onset:
        asp = ms(rng.uniform(25, 60))
        _paint(tr, pos, pos + asp, ndb=-18.0 + rng.uniform(-3, 3), nc=0.0)
        pos += asp
    return pos


def _fill_nan(a, default):
    ok = ~np.isnan(a)
    if not ok.any():
        return np.full_like(a, default)
    idx = np.arange(len(a))
    return np.interp(idx, idx[ok], a[ok])


def _smooth(a, frames):
    if frames <= 1:
        return a
    k = np.ones(frames) / frames
    pad = np.concatenate([np.full(frames, a[0]), a, np.full(frames, a[-1])])
    return np.convolve(pad, k, mode="same")[frames:-frames]


def _db_to_lin(db):
    out = np.zeros_like(db)
    ok = np.isfinite(db)
    out[ok] = 10.0 ** (db[ok] / 20.0)
    return out


def _f0_track(rng, spk, tr):
    f0 = np.full(tr.n, spk.f0)
    decl = np.where(tr.phrase >= 0, 1.12 - 0.24 * tr.phrase_pos, 1.0)
    t = np.arange(tr.n) * CTRL / FS
    wander = 1.0 + 0.03 * np.sin(2 * np.pi * rng.uniform(0.2, 0.7) * t + rng.uniform(0, 6.3)) \
        + 0.02 * np.sin(2 * np.pi * rng.uniform(1.0, 2.5) * t + rng.uniform(0, 6.3))
    accent = 1.0 + _smooth(tr.stress, 24)
    return f0 * decl * wander * accent


def _envelopes(tr, spk, rng, n_samples):
    """Per-STFT-frame voiced and noise envelopes (frames, bins), unit mean square."""
    nfr = (n_samples + STFT_FRAME) // STFT_HOP + 1
    centres = (np.arange(nfr) * STFT_HOP - STFT_FRAME // 2) / CTRL     # control-grid position
    centres = np.clip(centres, 0, tr.n - 1)
    ci = np.arange(tr.n)
    sm = 12                                                            # 30 ms coarticulation
    F = [np.interp(centres, ci, _smooth(_fill_nan(x, d), sm)) for x, d in zip(tr.F, (500, 1500, 2500, spk.f4, spk.f5))]
    B = [np.interp(centres, ci, _smooth(_fill_nan(x, d), sm)) for x, d in zip(tr.B, (80, 100, 150, 200, 260))]
    A = [np.interp(centres, ci, _smooth(_fill_nan(x, d), sm)) for x, d in zip(tr.A, (1.0, 0.7, 0.4, 0.25, 0.15))]
    nc = np.interp(centres, ci, tr.nc)
    nw = np.interp(centres, ci, tr.nw)
    f = np.fft.rfftfreq(STFT_FRAME, 1.0 / FS)[None, :].astype(np.float32)
    form = np.zeros((nfr, f.shape[1]), np.float32)
    for Fi, Bi, Ai in zip(F, B, A):
        u = (f - Fi[:, None].astype(np.float32)) * (2.0 / Bi[:, None].astype(np.float32))
        form += (Ai[:, None].astype(np.float32) ** 2) / (1.0 + u * u)
    form = np.sqrt(form + 0.02 ** 2)
    tilt = (1.0 + (f / 200.0) ** 2) ** (-spk.tilt / 12.04)
    voiced = form * tilt
    asp = form * (1.0 + (f / 500.0) ** 2) ** (-3.0 / 12.04)
    fric = 1.0 / np.sqrt(1.0 + ((f - nc[:, None]) / (0.5 * np.maximum(nw[:, None], 100.0))) ** 2)
    fric *= f ** 2 / (f ** 2 + 900.0 ** 2)
    noise = np.where(nc[:, None] > 0, fric, asp)

    def unit(e):
        return e / np.sqrt(np.mean(e ** 2, axis=1, keepdims=True) + 1e-20)

    return unit(voiced), unit(noise)


def _stft_filter(x, env):
    n = len(x)
    win = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(STFT_FRAME) / STFT_FRAME))
    nfr = env.shape[0]
    total = nfr * STFT_HOP + STFT_FRAME
    padded = np.zeros(total)
    padded[STFT_FRAME:STFT_FRAME + n] = x
    idx = np.arange(nfr)[:, None] * STFT_HOP + np.arange(STFT_FRAME)[None, :]
    spec = np.fft.rfft(padded[idx] * win, axis=1) * env
    y = np.fft.irfft(spec, n=STFT_FRAME, axis=1) * win
    out = np.zeros(total)
    for j in range(STFT_FRAME // STFT_HOP):
        part = y[j::STFT_FRAME // STFT_HOP]
        start = j * STFT_HOP
        out[start:start + part.size] += part.reshape(-1)
    return out[STFT_FRAME:STFT_FRAME + n] / 2.0


def speech(rng, n, continuous=False, speaker=None):
    """n samples of speech-like signal from one speaker."""
    spk = speaker or Speaker(rng)
    frames = n // CTRL + 2
    tr = build_utterance(rng, spk, frames, continuous)
    f0c = _f0_track(rng, spk, tr)
    vamp_c = _smooth(_db_to_lin(tr.vdb), 2)
    namp_c = _smooth(_db_to_lin(tr.ndb), 2)
    ts = np.arange(n) / CTRL
    ci = np.arange(frames)
    held = np.repeat(rng.normal(0.0, spk.jitter, frames // 2 + 2), 2)[:frames]
    f0s = np.interp(ts, ci, f0c * (1.0 + held))
    vamp = np.interp(ts, ci, vamp_c)
    namp = np.interp(ts, ci, namp_c)
    cum = np.cumsum(f0s / FS)
    pulses = np.nonzero(np.diff(np.floor(cum)) > 0)[0] + 1
    exc = np.zeros(n)
    exc[pulses] = np.sqrt(FS / f0s[pulses]) * vamp[pulses] * (1.0 + rng.normal(0.0, spk.shimmer, len(pulses)))
    noise = rng.standard_normal(n) * namp
    ev, en = _envelopes(tr, spk, rng, n)
    return _stft_filter(exc, ev) + _stft_filter(noise, en)


# ---- noise ------------------------------------------------------------------------
def _shaped(rng, n, shape):
    f = np.fft.rfftfreq(n, 1.0 / FS)
    y = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) * shape(np.maximum(f, 1.0)), n=n)
    return y / (np.sqrt(np.mean(y ** 2)) + 1e-20)


def _wander(rng, n, depth_db):
    t = np.arange(n) / FS
    db = depth_db * (np.sin(2 * np.pi * rng.uniform(0.05, 0.5) * t + rng.uniform(0, 6.3))
                     + 0.5 * np.sin(2 * np.pi * rng.uniform(0.3, 1.5) * t + rng.uniform(0, 6.3))) / 1.5
    return 10.0 ** (db / 20.0)


def white(rng, n):
    return _shaped(rng, n, lambda f: np.ones_like(f)) * _wander(rng, n, rng.uniform(0, 3))


def pink(rng, n):
    return _shaped(rng, n, lambda f: 1.0 / np.sqrt(np.maximum(f, 20.0))) * _wander(rng, n, rng.uniform(0, 4))


def brown(rng, n):
    return _shaped(rng, n, lambda f: 1.0 / np.maximum(f, 20.0)) * _wander(rng, n, rng.uniform(0, 4))


def fan(rng, n):
    fc = rng.uniform(250, 2000)
    hump_f, hump_w = rng.uniform(80, 600), rng.uniform(50, 400)
    broad = _shaped(rng, n, lambda f: 1.0 / np.sqrt(1.0 + (f / fc) ** 2)
                    + 1.5 / (1.0 + ((f - hump_f) / hump_w) ** 2))
    t = np.arange(n) / FS
    mains = rng.choice([50.0, 60.0])
    hum = np.zeros(n)
    kmax = int(rng.uniform(4, 2000.0 / mains))
    alpha = rng.uniform(0.5, 1.5)
    drift = 1.0 + 0.0005 * np.sin(2 * np.pi * 0.1 * t + rng.uniform(0, 6.3))
    for k in range(1, kmax + 1):
        a = rng.uniform(0.2, 1.0) / k ** alpha * (2.0 if k == 2 else 1.0)
        hum += a * np.sin(2 * np.pi * k * mains * drift * t + rng.uniform(0, 6.3))
    hum /= np.sqrt(np.mean(hum ** 2)) + 1e-20
    blade_f = rng.uniform(60, 400)
    blade = sum(rng.uniform(0.3, 1.0) / k * np.sin(2 * np.pi * k * blade_f * t + rng.uniform(0, 6.3)) for k in range(1, 5))
    blade *= 1.0 + 0.3 * np.sin(2 * np.pi * rng.uniform(0.5, 4) * t)
    blade /= np.sqrt(np.mean(blade ** 2)) + 1e-20
    mix = broad + 10 ** (rng.uniform(-8, 6) / 20) * hum + 10 ** (rng.uniform(-14, 0) / 20) * blade
    return mix / (np.sqrt(np.mean(mix ** 2)) + 1e-20)


def keyboard(rng, n):
    out = np.zeros(n)
    t = rng.uniform(0.0, 0.8)
    while t < n / FS:
        burst_end = t + rng.uniform(0.5, 4.0)
        rate = rng.uniform(4.0, 12.0)
        while t < min(burst_end, n / FS):
            for updown in (0, 1):
                start = int((t + updown * rng.uniform(0.05, 0.12)) * FS)
                length = int(rng.uniform(0.008, 0.03) * FS)
                if start + length >= n:
                    continue
                tau = rng.uniform(0.001, 0.006) * FS
                click = rng.standard_normal(length) * np.exp(-np.arange(length) / tau)
                fcl = rng.uniform(1500, 7000)
                q = rng.uniform(0.7, 4.0)
                spec = np.fft.rfft(click, 2 * length)
                f = np.fft.rfftfreq(2 * length, 1.0 / FS)
                spec *= 0.3 + 1.0 / (1.0 + ((f - fcl) / (fcl / q)) ** 2)
                click = np.fft.irfft(spec, 2 * length)[:length]
                level = rng.uniform(0.3, 1.0) * (0.6 if updown else 1.0)
                if rng.random() < 0.2:      # desk thump
                    tt = np.arange(length) / FS
                    click += 0.5 * np.sin(2 * np.pi * rng.uniform(80, 200) * tt) * np.exp(-tt / 0.008) * np.abs(click).max()
                out[start:start + length] += level * click / (np.abs(click).max() + 1e-20)
            t += rng.exponential(1.0 / rate) + 0.03
        t = burst_end + rng.uniform(0.3, 3.0)
    rms = np.sqrt(np.mean(out ** 2))
    return out / (rms + 1e-20) if rms > 0 else out


def babble(rng, n):
    voices = int(rng.integers(3, 9))
    out = np.zeros(n)
    for _ in range(voices):
        v = speech(rng, n, continuous=True)
        v /= np.sqrt(np.mean(v ** 2)) + 1e-20
        out += v * 10 ** (rng.uniform(-4, 4) / 20)
    if rng.random() < 0.5:                      # distant: duller
        fc = rng.uniform(2000, 5000)
        spec = np.fft.rfft(out)
        f = np.fft.rfftfreq(n, 1.0 / FS)
        out = np.fft.irfft(spec / np.sqrt(1.0 + (f / fc) ** 4), n=n)
    return out / (np.sqrt(np.mean(out ** 2)) + 1e-20)


NOISES = {"white": white, "pink": pink, "brown": brown, "fan": fan, "keyboard": keyboard, "babble": babble}
NOISE_WEIGHTS = {"white": 0.6, "pink": 1.0, "brown": 0.8, "fan": 1.2, "keyboard": 1.0, "babble": 1.0}


def random_eq(rng, x, depth_db=6.0):
    """A smooth random EQ (+-depth over log frequency) in the FFT domain."""
    n = len(x)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    pts = np.log2(np.array([20.0, 150.0, 500.0, 1500.0, 4000.0, 10000.0, 24000.0]))
    gains = rng.uniform(-depth_db, depth_db, len(pts))
    g = np.interp(np.log2(np.maximum(f, 20.0)), pts, gains)
    return np.fft.irfft(np.fft.rfft(x) * 10 ** (g / 20.0), n=n)


def band_limit(x, lo_hz, hi_hz):
    n = len(x)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    resp = np.ones_like(f)
    if hi_hz:
        resp *= 1.0 / np.sqrt(1.0 + (f / hi_hz) ** 16)
    if lo_hz:
        resp *= (f / lo_hz) ** 2 / np.sqrt(1.0 + (f / lo_hz) ** 4)
    return np.fft.irfft(np.fft.rfft(x) * resp, n=n)


def active_rms(s, frame=480, rel_db=-40.0):
    m = len(s) // frame
    if m == 0:
        return float(np.sqrt(np.mean(s ** 2)))
    e = np.mean(s[:m * frame].reshape(m, frame) ** 2, axis=1)
    if e.max() <= 0:
        return 0.0
    act = e > e.max() * 10 ** (rel_db / 10)
    return float(np.sqrt(e[act].mean()))


def make_clip(seed, seconds, noise_types=None, snr_db=None, clean_prob=0.07, noise_only_prob=0.04):
    """Returns (speech, noise, meta). speech + noise is the input; speech is the target."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    kind = "mix"
    u = rng.random()
    if noise_types is None:
        if u < noise_only_prob:
            kind = "noise-only"
        elif u < noise_only_prob + clean_prob:
            kind = "clean"
    s = np.zeros(n) if kind == "noise-only" else speech(rng, n)
    if noise_types is None:
        names = list(NOISES)
        w = np.array([NOISE_WEIGHTS[k] for k in names])
        count = 2 if rng.random() < 0.3 else 1
        noise_types = list(rng.choice(names, size=count, replace=False, p=w / w.sum()))
    noise = np.zeros(n)
    for name in noise_types:
        noise += NOISES[name](rng, n) * 10 ** (rng.uniform(-6, 0) / 20)
    if rng.random() < 0.5:
        s = random_eq(rng, s)
    if rng.random() < 0.5:
        noise = random_eq(rng, noise)
    if kind == "clean":
        snr = rng.uniform(35.0, 50.0)
        noise = pink(rng, n)
    else:
        snr = snr_db if snr_db is not None else rng.uniform(-5.0, 20.0)
    level = rng.uniform(-42.0, -18.0)
    if kind == "noise-only":
        noise *= 10 ** (rng.uniform(-55.0, -25.0) / 20) / (np.sqrt(np.mean(noise ** 2)) + 1e-20)
    else:
        sr = active_rms(s)
        s *= 10 ** (level / 20) / (sr + 1e-20)
        # SNR over the speech-active frames
        m = n // 480
        e = np.mean(s[:m * 480].reshape(m, 480) ** 2, axis=1)
        act = np.repeat(e > e.max() * 1e-4, 480)
        nr = np.sqrt(np.mean(noise[:m * 480][act] ** 2)) + 1e-20
        noise *= 10 ** ((level - snr) / 20) / nr
    lo = rng.uniform(40, 150) if rng.random() < 0.5 else 0.0
    hi = rng.choice([4000.0, 7000.0, 8000.0, 12000.0, 16000.0]) if rng.random() < 0.35 else 0.0
    if lo or hi:
        s, noise = band_limit(s, lo, hi), band_limit(noise, lo, hi)
    peak = np.max(np.abs(s + noise))
    if peak > 0.95:
        s, noise = s * 0.95 / peak, noise * 0.95 / peak
    meta = {"seed": int(seed), "kind": kind, "noise": "+".join(noise_types) if kind != "clean" else "pink(floor)",
            "snr_db": float(snr), "level_dbfs": float(level), "lowcut_hz": float(lo), "bandwidth_hz": float(hi)}
    return s, noise, meta
