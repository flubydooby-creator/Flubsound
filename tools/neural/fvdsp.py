"""Flubsound voice cleanup - the signal processing the model is trained on.

This file is the reference for the C++ side and must stay in step with it:
  * core/include/flub/neural/BandGains.h   (window, band layout, bin <-> band maps)
  * core/src/neural/VoiceCleanupRunner.cpp (features: band log-energies + voicing)
  * core/src/neural/AsyncModelProcessor.cpp (the BandGains renderer)

Feature set 1 ("flub-voice-v1"), 48 kHz only:
  hop N = 240 samples (5 ms), window 2N = 480 samples (a Vorbis power-
  complementary window), zero-padded to a 512-point FFT. Frame k analyses the
  samples [(k-1)N, (k+1)N) (zeros before the stream starts).
  22 bands with triangular weights between the centre frequencies below
  (RNNoise's layout, in Hz); a bin at or above the last centre belongs to the
  last band with weight 1. The same weights interpolate band gains to bins.
  Features per frame (23):
    0..21  log10 (band energy + 1e-10), energy = sum over bins of |X_k|^2 * weight
    22     voicing: the largest normalised autocorrelation, for lags of
           2.5 .. 15 ms, of the last 20 ms of a 4x box-decimated signal (12 kHz)
numpy only (the standard library for files); float64 throughout.
"""
import numpy as np

SAMPLE_RATE = 48000
HOP = 240
WIN = 2 * HOP
FFT = 512
NBINS = FFT // 2 + 1
BAND_CENTRES_HZ = np.array([0, 200, 400, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800, 3200,
                            4000, 4800, 5600, 6800, 8000, 9600, 12000, 15600, 20000], dtype=np.float64)
NBANDS = len(BAND_CENTRES_HZ)
FEATURE_SET = 1
NUM_FEATURES = NBANDS + 1
ENERGY_FLOOR = 1e-10

# Voicing feature (decimated domain, 12 kHz).
DECIMATION = 4
PITCH_SEGMENT = 240          # 20 ms at 12 kHz
LAG_MIN, LAG_MAX = 30, 180   # 2.5 .. 15 ms (400 .. 67 Hz)
PITCH_HISTORY = PITCH_SEGMENT + LAG_MAX
VOICING_EPS = 1e-9


def vorbis_window(n=WIN):
    i = np.arange(n, dtype=np.float64) + 0.5
    return np.sin(0.5 * np.pi * np.sin(np.pi * i / n) ** 2)


WINDOW = vorbis_window()


def band_interpolation(sample_rate=SAMPLE_RATE, fft=FFT, centres=BAND_CENTRES_HZ):
    """lo[k], frac[k]: bin k lies between band lo and lo + 1 (weights 1 - frac, frac)."""
    nb = len(centres)
    nbins = fft // 2 + 1
    lo = np.zeros(nbins, dtype=np.int64)
    frac = np.zeros(nbins, dtype=np.float64)
    for k in range(nbins):
        f = k * (sample_rate / fft)
        if f >= centres[-1]:
            lo[k], frac[k] = nb - 2, 1.0
            continue
        if f <= centres[0]:
            lo[k], frac[k] = 0, 0.0
            continue
        b = int(np.searchsorted(centres, f, side="right") - 1)
        lo[k] = b
        frac[k] = (f - centres[b]) / (centres[b + 1] - centres[b])
    return lo, frac


BAND_LO, BAND_FRAC = band_interpolation()


def band_matrix():
    """(NBINS, NBANDS) weights: energies = P @ M, bin gains = g @ M.T."""
    m = np.zeros((NBINS, NBANDS))
    m[np.arange(NBINS), BAND_LO] += 1.0 - BAND_FRAC
    m[np.arange(NBINS), BAND_LO + 1] += BAND_FRAC
    return m


BAND_MATRIX = band_matrix()


def frame_count(n_samples):
    return (n_samples + HOP - 1) // HOP


def stft_frames(x):
    """Complex spectra (frames, NBINS) of the windows [(k-1)N, (k+1)N)."""
    x = np.asarray(x, dtype=np.float64)
    nf = frame_count(len(x))
    padded = np.zeros(HOP + nf * HOP + HOP)
    padded[HOP:HOP + len(x)] = x
    idx = np.arange(nf)[:, None] * HOP + np.arange(WIN)[None, :]
    frames = padded[idx] * WINDOW[None, :]
    buf = np.zeros((nf, FFT))
    buf[:, :WIN] = frames
    return np.fft.rfft(buf, axis=1)


def band_energies(spec):
    return (np.abs(spec) ** 2) @ BAND_MATRIX


def decimate(x):
    x = np.asarray(x, dtype=np.float64)
    nf = frame_count(len(x))
    padded = np.zeros(nf * HOP)
    padded[:len(x)] = x
    return padded.reshape(-1, DECIMATION).mean(axis=1)


def voicing(x):
    """Feature 22 for every frame of x."""
    d = decimate(x)
    nf = frame_count(len(x))
    per_hop = HOP // DECIMATION
    hist = np.zeros(PITCH_HISTORY + nf * per_hop)
    hist[PITCH_HISTORY:] = d
    # history of frame k: the PITCH_HISTORY decimated samples ending with hop k
    idx = (np.arange(nf)[:, None] + 1) * per_hop + np.arange(PITCH_HISTORY)[None, :]
    h = hist[idx]
    seg = h[:, LAG_MAX:]
    e_seg = np.einsum("fn,fn->f", seg, seg)
    # num[m] = sum_n seg[n] h[m + n] for m = LAG_MAX - lag (an FFT cross-correlation, no wrap at 512)
    L = 512
    c = np.fft.irfft(np.conj(np.fft.rfft(seg, L, axis=1)) * np.fft.rfft(h, L, axis=1), L, axis=1)
    m = LAG_MAX - np.arange(LAG_MIN, LAG_MAX + 1)
    num = c[:, m]
    cs = np.concatenate([np.zeros((nf, 1)), np.cumsum(h * h, axis=1)], axis=1)
    e_ref = cs[:, m + PITCH_SEGMENT] - cs[:, m]
    r = num / np.sqrt(e_seg[:, None] * np.maximum(e_ref, 0.0) + VOICING_EPS)
    return np.maximum(r.max(axis=1), 0.0)


def features_from_spec(spec, x):
    e = band_energies(spec)
    f = np.empty((spec.shape[0], NUM_FEATURES))
    f[:, :NBANDS] = np.log10(e + ENERGY_FLOOR)
    f[:, NBANDS] = voicing(x)
    return f


def features(x):
    return features_from_spec(stft_frames(x), x)


def render(x, gains):
    """Applies per-frame band gains (frames, NBANDS) with the BandGains renderer
    (window, zero-padded FFT, bin interpolation, synthesis window, overlap-add)
    and returns the output aligned with x (the renderer's own frame of latency
    removed)."""
    x = np.asarray(x, dtype=np.float64)
    # one hop of zeros at the end, so the last hop gets both of its windows
    spec = stft_frames(np.concatenate([x, np.zeros(HOP)]))
    nf = spec.shape[0]
    gains = np.asarray(gains, dtype=np.float64)
    if len(gains) < nf:
        gains = np.vstack([gains, np.repeat(gains[-1:], nf - len(gains), axis=0)])
    bin_gains = gains[:nf] @ BAND_MATRIX.T
    y = np.fft.irfft(spec * bin_gains, n=FFT, axis=1)[:, :WIN] * WINDOW[None, :]
    out = np.zeros(HOP + nf * HOP + HOP)
    out[:nf * HOP] += y[:, :HOP].reshape(-1)
    out[HOP:HOP + nf * HOP] += y[:, HOP:].reshape(-1)
    # frame k's window starts at sample (k-1)N: shift by one hop
    return out[HOP:HOP + len(x)]


def wiener_targets(spec_speech, spec_noise):
    """Ideal band gains sqrt(Es / (Es + En)) and the mask of bands with any energy."""
    es = band_energies(spec_speech)
    en = band_energies(spec_noise)
    tot = es + en
    mask = tot > 1e-12
    g = np.where(mask, np.sqrt(np.clip(es / np.maximum(tot, 1e-30), 0.0, 1.0)), 0.0)
    return g, mask


def vad_targets(spec_speech, rel_db=-40.0, abs_floor=1e-9):
    es = band_energies(spec_speech).sum(axis=1)
    if es.max() <= 0:
        return np.zeros(len(es))
    active = (es > es.max() * 10 ** (rel_db / 10)) & (es > abs_floor)
    # widen by two frames on each side: onsets and releases count as speech
    a = active.copy()
    for s in (1, 2):
        a[s:] |= active[:-s]
        a[:-s] |= active[s:]
    return a.astype(np.float64)


# ---- WAV I/O (standard library) -------------------------------------------------
def write_wav(path, data, rate=SAMPLE_RATE, bits=16):
    """data: (n,) or (n, ch) float in [-1, 1]."""
    import wave
    a = np.asarray(data, dtype=np.float64)
    if a.ndim == 1:
        a = a[:, None]
    if bits == 16:
        pcm = np.clip(np.round(a * 32767.0), -32768, 32767).astype("<i2")
    else:
        raise ValueError("16-bit only")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(a.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def write_wav_float(path, data, rate=SAMPLE_RATE):
    """32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT) via struct; data (n,) or (n, ch)."""
    import struct
    a = np.asarray(data, dtype=np.float32)
    if a.ndim == 1:
        a = a[:, None]
    ch = a.shape[1]
    payload = a.astype("<f4").tobytes()
    fmt = struct.pack("<HHIIHH", 3, ch, rate, rate * ch * 4, ch * 4, 32)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(payload)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(payload)) + payload)


def read_wav(path):
    """Returns (float64 array (n, ch), rate) for 16-bit PCM or 32-bit float WAV."""
    import struct
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and len(body) >= 26:
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 1 and bits == 16:
        a = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        a = v.astype(np.float64) / float(1 << 23)
    elif tag == 3 and bits == 32:
        a = np.frombuffer(data, dtype="<f4").astype(np.float64)
    else:
        raise ValueError(f"{path}: unsupported WAV format {tag}/{bits}")
    return a.reshape(-1, ch), rate
