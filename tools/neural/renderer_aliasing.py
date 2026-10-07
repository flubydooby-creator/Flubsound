"""Flubsound voice cleanup - time aliasing of the BandGains renderer (docs/03 §16.3).

The renderer (AsyncModelProcessor::renderBands, fvdsp.render) windows each
480-sample frame, zero-pads it to 512, multiplies its spectrum by the bin gains
and transforms back: a 512-point circular convolution with the gains' zero-phase
response (centred on sample 0, spreading both ways). It keeps samples 0 .. 479
and discards 480 .. 511, so whatever of the response reaches past either end of
the frame wraps into the other end, attenuated only by the synthesis window.

This script measures that wrap: white noise through the renderer, against the
same frames convolved linearly with the same response (taps at lags -256 .. 255),
windowed and overlap-added alike. The error is their difference, in dB relative
to the linear output. numpy only.

    python tools/neural/renderer_aliasing.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fvdsp as fv  # noqa: E402

N, W, F = fv.HOP, fv.WIN, fv.FFT
FLOOR = 0.03  # the model's lowest band gain (VoiceCleanupRunner, -30.5 dB)


def wrap_error_db(band_gains, rng, frames=400):
    bin_gains = fv.BAND_MATRIX @ np.asarray(band_gains, dtype=np.float64)
    response = np.fft.irfft(bin_gains, n=F)  # zero-phase, 512-periodic
    lags = np.arange(-F // 2, F // 2)
    taps = response[lags % F]
    x = rng.standard_normal(N * (frames + 1))
    circular = np.zeros(N * (frames + 2))
    linear = np.zeros_like(circular)
    for k in range(frames):
        u = x[k * N:k * N + W] * fv.WINDOW
        circular[k * N:k * N + W] += np.fft.irfft(np.fft.rfft(u, F) * bin_gains, n=F)[:W] * fv.WINDOW
        full = np.convolve(u, taps)  # index j is lag j + lags[0]
        linear[k * N:k * N + W] += full[-lags[0]:-lags[0] + W] * fv.WINDOW
    s = slice(N, N * frames)  # every sample has both of its windows
    err = circular[s] - linear[s]
    return 10.0 * np.log10(max(np.sum(err ** 2), 1e-300) / np.sum(linear[s] ** 2))


def main():
    rng = np.random.default_rng(1)
    nb = fv.NBANDS
    centres = fv.BAND_CENTRES_HZ
    cases = [
        ("unity", np.ones(nb)),
        ("alternate bands 1 / 0.03 (the sharpest curve)", np.array([1.0 if b % 2 == 0 else FLOOR for b in range(nb)])),
        ("alternate bands 1 / 0.03 below 1.6 kHz, 1 above", np.array([(1.0 if b % 2 == 0 else FLOOR) if b < 9 else 1.0 for b in range(nb)])),
        ("step: 1 below 1 kHz, 0.03 above", np.array([1.0 if c < 1000 else FLOOR for c in centres])),
        ("speech band: 1 at 200 - 3200 Hz, 0.03 elsewhere", np.array([1.0 if 200 <= c <= 3200 else FLOOR for c in centres])),
    ]
    for name, g in cases:
        print(f"{name:50s} wrap error {wrap_error_db(g, rng):7.1f} dB")
    draws = [wrap_error_db(rng.uniform(FLOOR, 1.0, nb), rng, frames=150) for _ in range(40)]
    print(f"{'random band gains in [0.03, 1] (40 draws)':50s} median {np.median(draws):6.1f} dB, worst {np.max(draws):6.1f} dB")


if __name__ == "__main__":
    main()
