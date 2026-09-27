# 03 — DSP Design

> Every sample-touching algorithm in Flubsound Pro lives in `flub_core` (`core/`): allocation-free C++20 with no third-party code, exercised by the zero-dependency unit tests in `tests/`. This document covers **every DSP module of the processing chain**, as implemented:
> - the signal flow through the chain and through each module;
> - the maths as implemented: exact formulas and constants;
> - the parameter keys, ranges and defaults from [`core/src/engine/Parameters.cpp`](../core/src/engine/Parameters.cpp);
> - the smoothing that makes every change click-free;
> - latency and CPU, per module (sections 2–13) and for the whole chain (section 15);
> - how Gaming and Music mode use each module, and the macros and protection loops behind them (section 14);
> - the tests that prove each property;
> - known limitations. Roadmap items are labelled as such.
>
> Section 0 describes the shared primitives that the module sections build on, and section 1 the gain staging across the whole chain.

**Reading conventions**

- Levels are dBFS *peak* unless stated otherwise. For a sine, RMS = peak − 3.01 dB.
- `fs` is the session sample rate. The *control rate* is `fs / 16`: the parametric EQ, dynamic EQ, bass engine, clarity enhancer and headphone virtualizer use `kControlInterval = 16`; the stereo spatializer uses 32 (section 7). The other modules run their smoothing per sample (compressor, limiter, maximizer, saturator, transient shaper) or per STFT hop (noise gate).
- `x'` means "the smoothed value of parameter x".
- **Measured** figures were reproduced for this document with small harnesses linked against the module sources. The unit tests assert looser bounds, which are listed per module.
- **CPU** figures are indicative only, measured on one machine: g++ 13.3 `-O2`, x86-64 (Intel Xeon @ 2.10 GHz, container), 48 kHz, stereo, 512-sample blocks, white-noise input, `ScopedNoDenormals` active, best of 5 runs. "% core" = ns per stereo sample ÷ 20 833 ns (one 48 kHz sample period). Section 15 has the chain-level summary.

| § | Section | Main sources |
|---|---|---|
| [0](#0-conventions--shared-primitives) | Conventions & shared primitives | `core/include/flub/common/*`, `core/include/flub/dsp/{Svf,Biquad,Crossover,EnvelopeFollower,Oversampler,TruePeakDetector,FirDesign,Fft}.h`, `core/src/dsp/{Oversampler,Fft}.cpp` |
| [1](#1-gain-staging-overview) | Gain staging overview | `core/src/engine/ProcessingChain.cpp` |
| [2](#2-parametric-eq) | Parametric EQ | `core/src/dsp/ParametricEq.cpp` |
| [3](#3-dynamic-eq) | Dynamic EQ | `core/src/dsp/DynamicEq.cpp` |
| [4](#4-bass-engine) | Bass engine | `core/src/dsp/BassEngine.cpp` |
| [5](#5-transient-shaper--clarity-enhancer) | Transient shaper + Clarity enhancer | `core/src/dsp/TransientShaper.cpp`, `core/src/dsp/ClarityEnhancer.cpp` |
| [6](#6-saturation) | Saturation | `core/src/dsp/Saturator.cpp` |
| [7](#7-stereo-widener--spatializer) | Stereo widener / spatializer | `core/src/dsp/StereoSpatializer.cpp` |
| [8](#8-headphone-virtualizer) | Headphone virtualizer | `core/src/dsp/HeadphoneVirtualizer.cpp` |
| [9](#9-look-ahead-compressor-downward--upward) | Look-ahead compressor (downward + upward) | `core/src/dsp/Compressor.cpp` |
| [10](#10-true-peak-limiter) | True-peak limiter | `core/src/dsp/TruePeakLimiter.cpp` |
| [11](#11-loudness-maximizer--soft-clipper) | Loudness maximizer + soft clipper | `core/src/dsp/LoudnessMaximizer.cpp` |
| [12](#12-spectral-noise-gate) | Spectral noise gate | `core/src/dsp/SpectralNoiseGate.cpp` |
| [13](#13-metering-loudness-lra-true-peak-rms-and-correlation) | Metering: loudness, LRA, true peak, RMS and correlation | `core/src/analysis/LoudnessMeter.cpp`, `core/include/flub/analysis/{PeakMeters,LoudnessFollower,ChannelWeights}.h` |
| [14](#14-macros-musicgaming-modes--protection-loops) | Macros, Music/Gaming modes & protection loops | `core/src/engine/{MacroMap,ProcessingChain,Protection,MixEngine}.cpp` |
| [15](#15-chain-level-cpu-and-latency-summary) | Chain-level CPU and latency summary | `core/src/engine/{ProcessingChain,ModuleSlot,MixEngine}.cpp` |

---

## 0. Conventions & shared primitives

### 0.1 Numeric format and the module contract

| Aspect | Rule (as implemented) | Source |
|---|---|---|
| Sample format | 32-bit `float`, planar, processed **in place** on an `AudioBlock`: an array of up to `kMaxChannels = 8` channel pointers plus the sample count. Sub-blocks are views, so they cost no allocation. | `common/AudioBlock.h` |
| Double precision | Used where precision matters more than speed: <br>• filter *design* (`SvfCoeffs::make` computes in `double` and keeps `g`, `k` as `double` for exact response evaluation); <br>• FIR tap design (`FirDesign.h`, taps are then stored as `float`); <br>• `Biquad` coefficients and state (TDF-II); <br>• `MeanSquareFollower` state (so also the `LevelMeter` RMS and correlation and the `LoudnessFollower`s of the control loops); <br>• the loudness gates' sums of squares; <br>• `LoudnessMeter` energies (per-channel sums, the sub-block ring, the gating histogram); <br>• the `TruePeakLimiter` gain envelope (the release one-pole `gain` and its fast / slow / blend coefficients) and the box filter's running sum (`boxSum`, `boxLength`): a float one-pole with a long release stalls a few 1e−5 below its target; <br>• the `Compressor` gain computer's smoothing state (`gainDb`) and its attack, release and auto-release coefficients (the peak detector itself is float): a float one-pole with a coefficient near 1 − 1e−5 stalls up to ~0.7 dB short of its target; <br>• the `StereoSpatializer`'s output-correlation mean products. | `dsp/Svf.h`, `dsp/FirDesign.h`, `dsp/Biquad.h`, `dsp/EnvelopeFollower.h`, `analysis/LoudnessMeter.h`, `dsp/TruePeakLimiter.h`, `dsp/Compressor.h`, `dsp/StereoSpatializer.h`, `engine/Protection.h` |
| Threading | `prepare()` runs off the audio thread and may allocate. `reset()`, `process()` and all setters run on the audio thread: `noexcept`, no allocation, no locks, no I/O, bounded time. | `dsp/Processor.h` |
| Parameter push | `ProcessingChain::applyParameters()` calls every module setter **every block**. Each module sanitises the new values, compares them with the current set and returns early when nothing changed, so an unchanged parameter costs one comparison. | `engine/ProcessingChain.cpp` |
| Sanitising | **First line, the `ParameterStore`:** `ParameterStore::set()` ignores NaN and keeps the current value; `Info::clamp()` maps NaN to the parameter's default; ±inf clamps to the range edge. A NaN from automation, a host or a script therefore never reaches a module through the store (test *ParameterStore: NaN is ignored, infinities clamp, and the chain stays finite*). <br>**Second line, each module** (for direct API use): values are clamped to the module's documented range, and ±inf clamps to the range edge. NaN handling differs by module: <br>• the previous value is kept by `BassEngine`, `ClarityEnhancer` and `TransientShaper`; <br>• the default is used by `ParametricEq` and `Saturator`; <br>• `DynamicEq` keeps the previous setting for **any** non-finite value, including ±inf. | `engine/Parameters.h`, `src/engine/Parameters.cpp`, per-module `sanitise()` |
| Latency | An integer number of samples, constant between `prepare()` calls. The chain delay-compensates the bypass paths (`ModuleSlot`, global bypass). | `dsp/Processor.h`, `engine/ModuleSlot.h` |
| Channels | A block may carry fewer channels than prepared. Only the channels present are processed, and they are the only ones linked. | per module |

`DelayLine` (`common/DelayLine.h`) is the fixed integer delay used for the dry-path compensation of every latency-carrying stage. Block processing and per-sample processing (`processSample()` + `advance()`) give identical results (test *DelayLine: block delay equals per-sample delay*).

### 0.2 TPT state-variable filter (`dsp/Svf.h`)

The workhorse filter is Andrew Simper's (Cytomic) linear trapezoidal state-variable filter, a topology-preserving transform (TPT). It was chosen over direct-form biquads for three reasons:
- it stays well behaved when its coefficients change every few samples (dynamic EQ, glides);
- its two states are integrator outputs with a physical meaning;
- it is numerically robust for low cutoffs at high sample rates.

```
design:   w  = tan(pi * fc / fs)            fc clamped to [5 Hz, 0.49 fs], Q clamped to [0.025, 40]
          k  = 1 / Q                        A  = 10^(gainDb / 40)
          g  = w  (w / sqrt(A) for LowShelf, w * sqrt(A) for HighShelf)
          a1 = 1 / (1 + g (g + k)),  a2 = g a1,  a3 = g a2

per sample (state ic1, ic2):
          v3 = v0 - ic2
          v1 = a1 ic1 + a2 v3               band-pass core   s / (s^2 + k s + 1)
          v2 = ic2 + a2 ic1 + a3 v3         low-pass core    1 / (s^2 + k s + 1)
          ic1 = 2 v1 - ic1,   ic2 = 2 v2 - ic2
          y  = m0 v0 + m1 v1 + m2 v2
```

The mix coefficients select the response. Every type is one of the normalised analog prototypes below, evaluated at `s = j·tan(π f/fs)/g` (see the response mapping below):
- for the non-shelf types `g = tan(π fc/fs)`, so `|s| = 1` falls exactly on `fc`;
- for the shelves `g` is scaled by `1/√A` (low shelf) or `√A` (high shelf), which moves the half-gain point `|H| = A` onto `fc`.

| `FilterType` | `(m0, m1, m2)` | Analog prototype H(s) | Key property |
|---|---|---|---|
| `LowPass` | (0, 0, 1) | 1 / (s² + ks + 1) | −3.01 dB at fc for Q = 1/√2 |
| `HighPass` | (1, −k, −1) | s² / (s² + ks + 1) | |
| `BandPass` | (0, k, 0) | ks / (s² + ks + 1) | **0 dB at fc** (unity-gain detector) |
| `Notch` | (1, −k, 0) | (s² + 1) / (s² + ks + 1) | zero at fc |
| `AllPass` | (1, −2k, 0) | (s² − ks + 1) / (s² + ks + 1) | \|H\| = 1, −180° at fc |
| `Bell` (k = 1/(Q·A)) | (1, k(A² − 1), 0) | (s² + (A/Q)s + 1) / (s² + s/(QA) + 1) | A² = 10^(dB/20) at fc. A cut is the exact reciprocal of the equal boost, H₊·H₋ = 1. |
| `LowShelf` | (1, k(A − 1), A² − 1) | (s² + kAs + A²) / (s² + ks + 1) | full gain A² at DC, **half the dB gain (A) at fc**, 0 dB at HF |
| `HighShelf` | (A², kA(1 − A), 1 − A²) | (A²s² + kAs + 1) / (s² + ks + 1) | full gain at HF, half the dB gain at fc, 0 dB at DC |

**Exact response mapping.**
- The digital filter is the bilinear transform of the prototype, prewarped at the design frequency. Its response at frequency `f` is therefore exactly the prototype evaluated at `s = j·tan(ω/2)/g`, with `ω = 2π f / fs`.
- `SvfCoeffs::response()` evaluates this in `double`, with `f` clamped to [0, 0.4999 fs]:
  ```
  H(f) = m0 + m1 * s/(s^2+ks+1) + m2 * 1/(s^2+ks+1),   s = j tan(pi f / fs) / g
  ```
  `magnitudeDb()` floors `|H|` at 1e-12, i.e. −240 dB.
- The GUI curves (`ParametricEq::responseDb`) and several tests use exactly this function.
- Test *Svf: analytic response() matches the running filter for every type* checks the running filter against it to within 0.05 dB for all eight types, wherever the response is above −40 dB.

**Shelf convention (important for the bass engine).** The SVF shelf's *corner frequency is its half-gain point*. Two examples:
- a 9 dB low shelf at 60 Hz (Q 0.7) gives +8.85 dB at 20 Hz, **+7.33 dB at 40 Hz**, +4.50 dB at 60 Hz, +0.65 dB at 120 Hz and +0.10 dB at 200 Hz (analytic, identical at 44.1 and 48 kHz);
- test *Svf: shelves reach their gain at DC / Nyquist side* asserts +4 dB at the corner of an 8 dB shelf.

`svfTickRaw()` returns the raw `v1`/`v2` so that crossovers can form LP and HP from one state (HP = v0 − k·v1 − v2). `SvfFilter` is a convenience multichannel wrapper with shared coefficients.

### 0.3 Butterworth cascades

An order-2N Butterworth filter is built from N second-order SVF sections, all at the **same** cutoff, with the pole-pair Qs from `butterworthQ(N, k)`:

```
Q_k = 1 / (2 sin((2k + 1) pi / (4N))),   k = 0 .. N-1        (Svf.h)
```

| Sections N | Order / slope | Q₀ | Q₁ | Q₂ | Q₃ | Used for |
|---|---|---|---|---|---|---|
| 1 | 2nd, 12 dB/oct | 0.7071 | | | | 12 dB/oct EQ cuts; LR4 halves; bass harmonics post-HP and 25 Hz pre-HP; bass protection detector |
| 2 | 4th, 24 dB/oct | 1.3066 | 0.5412 | | | 24 dB/oct EQ cuts; bass subsonic HP4, replace-fundamental HP4, harmonics LP4s; clarity air HP4/LP4/HP4 |
| 3 | 6th, 36 dB/oct | 1.9319 | 0.7071 | 0.5176 | | 36 dB/oct EQ cuts |
| 4 | 8th, 48 dB/oct | 2.5629 | 0.9000 | 0.6013 | 0.5098 | 48 dB/oct EQ cuts |

Every cascade is maximally flat and exactly −3.01 dB at its cutoff. One octave into the stopband it reads −12.3 / −24.1 / −36.2 / −48.2 dB for N = 1..4 (analytic).

### 0.4 Linkwitz–Riley crossovers and all-pass alignment (`dsp/Crossover.h`)

`LinkwitzRiley4` derives both bands from **one** Butterworth design (Q = 1/√2), using three SVF states per channel. With D = s² + √2·s + 1:

```
split:  lp1 = v2,  hp1 = x - k v1 - v2              (one SVF tick)
low  = LP2(lp1)  = 1/D^2        high = HP2(hp1) = s^4/D^2

low + high = (1 + s^4)/D^2 = (s^2 - sqrt2 s + 1)/(s^2 + sqrt2 s + 1)
             because (s^2 + sqrt2 s + 1)(s^2 - sqrt2 s + 1) = s^4 + 1
```

The two bands sum to a second-order **all-pass**: flat magnitude, with phase passing −180° at the crossover. Each band is −6.02 dB at fc.

`LinkwitzRileyAllPass` is the SVF `AllPass` at the same frequency and Q. `ThreeBandSplitter` (used by the maximizer glue) passes the low band through the all-pass of the upper crossover:

```
low' = AP_hi · LP4_lo · x,   mid + high = AP_hi · HP4_lo · x   =>   low' + mid + high = AP_hi · AP_lo · x
```

so the three bands still sum to an all-pass. `BassEngine` uses the identical LR4 topology (`lr4Split()` on `svfTickRaw`), but owns the state so it can be flushed and recovered after a NaN; the shared class keeps its state private.

**Consequence used by the bass engine.** Because LP4 + HP4 is an all-pass that reaches −180° at the corner, crossfading *dry* against an LR4-processed path would notch the corner region. The bass engine therefore engages such stages "parked" at a subsonic corner (§4.3.6).

### 0.5 Biquad and FIR design helpers

- `Biquad` (`dsp/Biquad.h`) is a transposed direct-form II section in `double`, normalised to a0 = 1. It is used where coefficients are static and precision matters: BS.1770 K-weighting and the Brown–Duda head-shadow sections (sections 8 and 13).
- `BiquadCoeffs::fromAnalogFirstOrder(B0, B1, A0, A1, fs)` is the plain bilinear transform with K = 2·fs, *without* prewarping.
- `fir::kaiser`, `fir::sinc` and `fir::besselI0` (series, converged to 1e-14) are prepare-time helpers for the half-band and true-peak interpolators. `fir::lowpass()` (a normalised Kaiser-windowed low-pass) exists but no core module currently uses it.

### 0.6 Half-band oversampler (`dsp/Oversampler.h`, `src/dsp/Oversampler.cpp`)

**Purpose.** Every nonlinearity that can create harmonics above the original Nyquist runs oversampled: the saturation curves (§6) and the maximizer's soft clipper (section 11). This keeps those harmonics from aliasing. The oversampler must have an **integer** round-trip latency, so the dry paths can be compensated exactly.

**Delta oversampling (both users).** Neither user sends the programme itself through the round trip. The curve runs on the upsampled signal `x̂`, only its *deviation* `f(x̂) − x̂` is downsampled, and that band-limited deviation is added to the input delayed by exactly the round-trip latency (`DelayLine`):

```
y = x[n − L] + down( f(up(x)) − up(x) )          L = oversampler round-trip latency
```

Where the curve is linear (quiet material) the deviation vanishes and `y` is the delayed input, so the half-band passband droop below never reaches the programme. The alias rejection is unchanged, because the harmonics still pass the same downsampler. The latency is unchanged too.

**Design.**
- Each 2× stage is a linear-phase Kaiser-windowed half-band FIR with `4d + 1` taps, centre index `2d`.
- The centre tap is exactly 0.5.
- The taps at even offsets from the centre are exactly 0.
- The 2d odd taps, `h[2j+1] = 0.5·sinc((2j+1−2d)/2)·kaiser(2j+1, 4d+1, β)`, are rescaled to sum to 0.5, which gives exactly unity DC gain.

```
upsample   (per low-rate input x[i], history w[j] = x[i-j], j = 0..2d-1):
    out[2i]   = w[d]                              phase 0 = pure delay of d samples
    out[2i+1] = 2 * sum_j h[2j+1] * w[j]          phase 1 = interpolated
downsample (per low-rate output y[i], high-rate input v):
    y[i] = 0.5 * v[2i - 2d] + sum_j h[2j+1] * v[2i - (2j+1)]
round trip of one stage = d (up) + d (down) = 2d samples at the stage's lower rate
```

| Quality | Stage 1 (1× ↔ 2×) | Stage 2 (2× ↔ 4×) | 2× latency | 4× latency |
|---|---|---|---|---|
| `High` | d₁ = 16 → 65 taps, β = 9.0 | d₂ = 4 → 17 taps, β = 8.0 | 2d₁ = **32** | 2d₁ + d₂ = **36** |
| `Low` | d₁ = 8 → 33 taps, β = 4.5 | d₂ = 3 → 13 taps, β = 6.0 | **16** | **19** |
| factor 1 | pass-through (in place) | — | **0** | — |

Stage 2's round trip is 2d₂ samples at the 2× rate, which is d₂ base-rate samples. Hence `latency = 2·d₁ + d₂` for 4×.

**Filter performance.** The table below was computed from the actual tap design. "Image" is the level of the spectral image re unity gain (the worst value over the range, which is at its top edge). For the upsampler this is the image; for the downsampler it is the alias rejection of the same filter. The frequency axis is in base-rate `f/fs`: 0.375 = 18 kHz, 0.417 = 20 kHz at 48 kHz, and 0.4535 = 20 kHz at 44.1 kHz.

| Stage | Max passband deviation 0…0.375 / 0.417 / 0.4535 fs | Worst image rejection 0…0.375 / 0.417 / 0.4535 fs |
|---|---|---|
| Stage 1 High (65 taps) | 0.0002 / 0.004 / 0.50 dB | −91 / −67 / −25 dB |
| Stage 1 Low (33 taps) | 0.023 / 0.092 / 1.24 dB | −51.5 / −39.5 / −17.5 dB |
| Stage 2 High (17 taps) | 0.003 / 0.011 / 0.028 dB | −70 / −58 / −50 dB |
| Stage 2 Low (13 taps) | 0.016 / 0.035 / 0.078 dB | −55 / −48 / −41 dB |

- The header's "~90 dB (High) / ~50 dB (Low)" image rejection holds for content up to about 0.375 fs.
- A half-band filter's transition band is centred on the original Nyquist, so the top octave of a 44.1 kHz stream sits in it. Measured on a bare 2× `Oversampler` round trip at 44.1 kHz: −2.3 dB (Low) and −1.0 dB (High) at 20 kHz, and ≤ 0.03 dB up to 18 kHz. At 48 kHz, 20 kHz reads −0.18 dB (Low) and −0.01 dB (High). Because of delta oversampling (above), this droop applies only to the generated harmonics, not to the programme: a quiet 19.5 kHz tone keeps unity gain through the saturator and the clipper (`tests/test_transparency.cpp`, §6.3).
- Test *Oversampler: round trip reproduces the input delayed by the reported latency* asserts a round-trip SNR of ≥ 80 dB (High) and ≥ 45 dB (Low) for a 997 Hz sine at exactly the reported latency.

**Cost.**
- The loops do not exploit tap symmetry: 2d multiply-adds per base-rate sample for the interpolated upsampler phase, and 2d per base-rate output of the downsampler.
- `upsample()` returns a view on internal buffers, allocated in `prepare()` for `factor × maxBlockSize` samples. It clamps a block longer than `maxBlockSize`, and callers never exceed it.
- A minimum-phase / polyphase-IIR variant with lower latency and non-linear phase is noted in the header as the planned alternative. It is **roadmap**, not implemented.

### 0.7 4× true-peak interpolator (`dsp/TruePeakDetector.h`)

This is the BS.1770-4 Annex 2 approach: 4× oversampling, then take the peak.

- **Prototype filter.** A 161-tap Kaiser-windowed sinc (β = 5.0, centre 80), split into 4 polyphase phases of `kTapsPerPhase = 40` taps each.
- **Phase normalisation.** Each interpolating phase p = 1..3 is normalised to unity DC gain. Phase 0 is the original sample, because the prototype's zeros fall on every 4th tap.
- **Per call.** Each call pushes x[n] and returns `max |.|` over the positions { n−20, n−20+¼, n−20+½, n−20+¾ }. The constant delay is `kDelay = kTapsPerPhase / 2 = 20` base samples.
- **Accuracy** (computed from the float taps):
  - every phase stays within −0.010 / +0.017 dB up to 0.4 fs, and within −0.020 / +0.039 dB up to 0.4535 fs (20 kHz at 44.1 kHz, 21.8 kHz at 48 kHz), so full-band programme is never materially under-read;
  - the error against an ideal fractional delay stays below −54 dB up to 0.4 fs (the worst case is near 0.37 fs) and below −47 dB up to 0.4535 fs;
  - above that the phases roll off (−0.65 dB at 0.47 fs);
  - the small over-read near 0.45 fs is conservative for both users.

  | f / fs | 0.2 | 0.3 | 0.4 | 0.44 | 0.45 | 0.4535 | 0.47 |
  |---|---|---|---|---|---|---|---|
  | phases 1,3 / phase 2 | −0.001 / −0.001 dB | −0.001 / −0.003 | −0.001 / −0.002 | −0.007 / −0.015 | +0.016 / +0.032 | +0.019 / +0.039 | −0.31 / −0.65 |

  The previous design (97 taps, 24 per phase, β = 8, `kDelay` 12) dipped by up to 2 dB near 0.4535 fs; the header comment records the change.
- **Shared design.** The taps come from the static `TruePeakDetector::designPhaseTaps()` (the prototype above, `kKaiserBeta = 5.0`, per-phase unity DC gain; phase 0 is left to the delayed sample itself).
- **Users.** The design exists once, so the limiter and the meters can never read different peaks.
  - `TruePeakMeter` (`analysis/PeakMeters.h`) runs the detector itself and takes the plain maximum of the four grid points (§13.3).
  - The limiter's `RefinedPeakDetector` (`TruePeakLimiter.cpp`) calls the same `designPhaseTaps()` with the same `kDelay` and adds parabolic refinement of local maxima of the 4× sequence (§10.3.1, which also records why the limiter's former separate design was dropped).
- **Tests.**
  - *TruePeakDetector: finds the inter-sample peak of an fs/4 sine at 45 degrees*: sample peak −3.01 dB, detected 0 dB ± 0.15 dB.
  - *TruePeakDetector: phase 0 reproduces the input with kDelay samples of delay*.
  - *TruePeakDetector: full-band accuracy - a 20 kHz sine at 44.1 kHz reads its true amplitude*: 15, 18, 19.5 and 20 kHz sines at 44.1 and 48 kHz, four sampling phases each, read their amplitude within ±0.05 dB.

### 0.8 FFT (`dsp/Fft.h`, `src/dsp/Fft.cpp`)

- A radix-2, iterative, in-place complex FFT. Bit-reversal and twiddle tables (`e^{-j2πk/N}`, k < N/2) are built in `prepare()`.
- `inverse()` scales by 1/N.
- `forwardReal()` / `inverseReal()` run a full N-point complex transform through an internal scratch buffer. That is correct, but about 2× the cost of a true real FFT, and one instance must not be shared between threads.
- The only real-time user is the spectral noise gate (section 12).
- PFFFT or vDSP/IPP behind the same interface is **roadmap** (`docs/02-tech-stack.md`, `docs/07-roadmap.md` item 1.9).
- Test *Fft: forward/inverse round trip and a pure tone lands in one bin* covers it.

### 0.9 Envelope followers and smoothers

All one-pole time constants are 63 % rise times: `c = exp(−1 / (τ · rate))` (`onePoleCoeff`, `common/Math.h`), so 90 % takes 2.3 τ. `rate` is the rate at which the one-pole is stepped, fs or fs/16, and every module derives its coefficients for that rate.

| Primitive | Law | Used by |
|---|---|---|
| `EnvelopeFollower` | `env = x + c (env − x)`, with c = attack if x > env, else release (branching peak follower) | transient-shaper pairs; bass protection detector (10/150 ms); harmonics envelope (0.5/50 ms) |
| `GainSmoother` | `state = t + c (state − t)` in **dB**, with c = attack when the *detector level is rising* (see below) | dynamic EQ, de-mud, presence, compressor |
| `MeanSquareFollower` | `ms = x² + c (ms − x²)`, state in `double` | RMS meters (`analysis/PeakMeters.h`), loudness followers (`analysis/LoudnessFollower.h`). The clarity detectors use their own float mean squares with the same law. |
| `LinearSmoothedValue` | linear ramp over `max(1, ⌊fs·ms/1000⌋)` steps; the last step lands **exactly** on the target | gains, mixes, crossfades (end points must be exactly 0 or 1) |
| `OnePoleSmoother` | `cur = t + c (cur − t)`. It snaps to t when a step no longer changes `cur`, or when \|cur − t\| < 1e-6·(1 + \|t\|). | parameter glides (log-frequency, dB, log-Q) |
| `TransientShaper::PeakHold` | 4 buckets of L = ⌈(fs·T_min/1000)/3⌉ samples; output = max(current bucket, 3 closed buckets), so the window spans 3L+1 … 4L samples (≥ T_min) | ripple-free levels: 25 ms (shaper, bass detectors), 7.5 ms (air) |
| `SvfGlide` / `GGlide` | per-sample interpolation from the previous control-rate design to the new one (§0.10) | dynamic bells and shelves, corner sweeps |

**Attack/release convention** (`GainSmoother`, Giannoulis–Massberg–Reiss decoupled branching topology). "Attack" always means the response to the **detector level rising**:
- Compressor-type computers (level up → gain down: `CutAbove`, `BoostBelow`, de-mud, presence) use `expanderMode = false`: a *falling* gain uses the attack time.
- Expander-type computers (level up → gain up: `BoostAbove`, `CutBelow`) use `expanderMode = true`: a *rising* gain uses the attack time.

**Why peak holds.** A plain peak follower decays between waveform peaks, so it ripples at twice the signal frequency. Any gain derived from it then amplitude-modulates the audio, which is audible distortion on bass. The window covers at least half a period of the lowest frequency of interest: 25 ms is half the period of 20 Hz. So a steady tone always has a crest inside the window and reads as a constant, while a rising level still passes instantly. The float `OnePoleSmoother` stall fix (the "a step that no longer moves counts as settled" rule) is covered by test *OnePoleSmoother: glides converge exactly to non-zero targets (no float stall)*.

### 0.10 Control rate, coefficient glides and block-size invariance

**Control rate.** `ParametricEq`, `DynamicEq`, `BassEngine` and `ClarityEnhancer` run their parameter logic every `kControlInterval = 16` samples: smoothers, gain computers and filter redesign. (The spatializer uses 32; see section 7.) The control phase is counted in **absolute stream time**: the countdown survives across `process()` calls, and blocks are split at tick boundaries.

**Coefficient glides.** A new design is never switched in as a step. The filter glides to it per sample across the next 16 samples, using one of three schemes:

| Scheme | What is interpolated per sample | Used by |
|---|---|---|
| EQ ramp | the six coefficients (a1, a2, a3, m0, m1, m2), linearly from the previous design to the new one | `ParametricEq` |
| `SvfGlide` | (g, k, m0, m1, m2), with a1..a3 **re-derived** each sample, so every intermediate set is a valid, stable SVF | `DynamicEq` (own copy), bass shelf, de-mud and presence bells, air shelf |
| `GGlide` | only the prewarped frequency g, with k and m fixed (fixed-Q LP/HP/LR4 sweeps) and a1..a3 re-derived | bass subsonic, mono, replace, tighten and harmonics filters |

**Block-size invariance policy.**
- Output must not depend on how the host splits the stream. Tick positions and ramp positions are taken from the global control phase, never from the block start.
- Per-sample algorithms (`TransientShaper`, `Saturator`) are invariant by construction. The saturator splits segments only at crossfade ends and at `maxBlockSize`.
- The tests assert exact equality where the implementation is exact:
  - *DynamicEq review: bit-exact for any block split, with events at arbitrary samples* (random block sizes 1..4096 and 60 random events);
  - *BassEngine (review): every block size gives bit-identical output*;
  - *Clarity (review): every block size gives bit-identical output*;
  - *TransientShaper: output is independent of the host block size*.
- `ParametricEq` and `Saturator` assert ≤ 1e-5. The saturator differs only in *when* a sub-1e-15 state is flushed, which is about 1e-14 in effect.

### 0.11 Denormal and non-finite policy

**First line: the host.** Every real-time entry point holds a `ScopedNoDenormals` (`common/Denormals.h`):
- on x86 it sets MXCSR FTZ | DAZ (`0x8040`);
- on AArch64 it sets FPCR.FZ (bit 24);
- entry points: the app's audio callback (`app/Source/engine/AudioEngineHost.cpp`), the plug-in's `processBlock`, and the CLI's offline renderer and analysis.

**Second line: every module flushes its own recursive state.** A host that forgot FTZ therefore cannot trigger a 10–100× subnormal slowdown:

| Module | Flush floor | When | Non-finite recovery |
|---|---|---|---|
| `ParametricEq` | SVF states \|v\| < 1e-15 (−300 dBFS) | end of every processed segment | a non-finite state restarts that section from rest at the segment end |
| `DynamicEq` | SVF states < 1e-20; envelope < 1e-9 | every control tick | the per-band state sum is checked every tick; band state cleared |
| `BassEngine` | SVF states < 1e-20; envelopes < 1e-15 | every control tick | any non-finite state clears **all** states |
| `ClarityEnhancer` | SVF states < 1e-20; envelopes and mean squares < 1e-15 | every control tick | all states cleared |
| `TransientShaper` | +1e-5 (−100 dBFS) added to the detector input; NaN read as 0; input clamped to 1e6 | per sample | envelopes can never be poisoned |
| `Saturator` | IIR states < 1e-15 | end of every segment; the tape emphasis states also every 64 oversampled samples (`kTapeFlushInterval`) | non-finite states flushed to 0 at the same points |

**Chain-level guard.** `ProcessingChain::process()` multiplies every input sample by 0 and sums the results. If the sum is non-finite, the block is output as silence and the signal path is reset: every module, the virtualiser, the dry-path delay and limiter, the meters and the distortion monitor (`resetSignalState()`). The control loops (SafetyGovernor, AutoLevel, AutoDrive, LoudnessMatch) keep their state, since none of them has seen the block; a full reset would snap the governor scale back to 1 and the AutoLevel gain to 0 dB, i.e. seconds of harder-driven or louder audio after a single NaN (tests *Chain: a NaN/Inf input block is dropped and the chain recovers*, *Chain: a dropped NaN/Inf block resets the signal path but keeps the converged governor, AutoLevel and AutoDrive state*).

### 0.12 Tests that prove the primitives (`tests/test_primitives.cpp`)

*Svf: bell reaches its gain at the centre frequency and is flat far away* · *Svf: shelves reach their gain at DC / Nyquist side* · *Svf: analytic response() matches the running filter for every type* · *Svf: Butterworth Q table* · *LR4: bands sum to a flat magnitude; each band is -6 dB at the crossover* · *ThreeBandSplitter: low + mid + high is flat* · *Biquad: first-order analog BLT keeps DC and HF gains* · *Oversampler: round trip reproduces the input delayed by the reported latency* · *Oversampler: latency values per quality* (36 / 32 / 19 / 16 / 0) · *TruePeakDetector: finds the inter-sample peak of an fs/4 sine at 45 degrees* · *TruePeakDetector: phase 0 reproduces the input with kDelay samples of delay* · *TruePeakDetector: full-band accuracy - a 20 kHz sine at 44.1 kHz reads its true amplitude* · *Fft: forward/inverse round trip and a pure tone lands in one bin* · *SpscRing: FIFO order, capacity and overflow drop* · *DelayLine: block delay equals per-sample delay* · *OnePoleSmoother: glides converge exactly to non-zero targets (no float stall)*.

---

## 1. Gain staging overview

### 1.1 Purpose

Flubsound adds bass, presence, air, harmonics, transient punch, drive and loudness. Gain staging is what keeps those additions from stacking up into clipping, limiter pumping or THD, and it lets a user switch any module in or out without a level jump that is not the module's actual effect. The chain relies on five rules, all implemented in code:

1. **Neutral means exact.** At neutral settings every module described in §2–§6 is an exact identity; the saturator is exactly its delayed input. (For the bass engine "neutral" includes `bass.subsonic = 0`: its default 20 Hz high-pass is active.) A bypassed module (`ModuleSlot`) is exactly its latency-delayed dry path.
2. **Unity small-signal gain for everything nonlinear.**
   - The saturation curves are `f(g·x)/g` with `f'(0) = 1` (§6).
   - The Chebyshev bass harmonics and the air exciter add harmonics at a level proportional to the input band level and leave the fundamental untouched (§4, §5).
   - Quiet material therefore passes at unity gain.
3. **Every boost is explicit and bounded.** Level-dependent boosts are *protected* (bass shelf), *tapered* at the noise floor (dynamic-EQ `BoostBelow`, presence) or *gated* (de-mud).
4. **Loudness-adding macro contributions are governed.** The SafetyGovernor scales them back when the limiter or clipper work too hard (section 14).
5. **Exactly one stage guarantees the ceiling on the processed path:** the maximizer's true-peak limiter. Between stages the signal is float, may exceed 0 dBFS, and is never clipped. The only stage after the limiter, `output.gain`, can only attenuate (−24…0 dB). The global-bypass reference, which skips the maximizer, has its own true-peak limiter at the same ceiling while bypass is engaged (§14.5, §14.6), and the desktop app adds a master limiter after the strip sum.

### 1.2 Signal flow (where gain can be added or removed)

```
 in ─► input gain ±24 dB ─► AutoLevel ±12 dB (+1 / −4 dB/s) ─► virtualiser, or BS.775 downmix (×0.7071 overall)
                                                                (virt.on toggles crossfade the two folds over 20 ms)
    ─► [gate]        attenuation only (≤ 40 dB); in the chain only in the Quality latency profile
    ─► [EQ]          ±24 dB per band, output −24..+12 dB
    ─► [DynEQ]       static ±12 dB + dynamic up to ±24 dB per band (range), noise-floor tapered
    ─► [Bass]        low shelf 0..+15 dB, withdrawn to keep predicted LF peak ≤ bass.protect;
                     harmonics mix ≤ ×2 of the generated harmonics; subsonic HP removes DC/rumble
    ─► [Clarity]     transient gain ±12 (attack) ±12 (sustain) dB; presence ≤ +6 dB; de-mud ≤ −4 dB;
                     air: harmonics at ≤ −12 dB re band + ≤ +2 dB shelf
    ─► [Saturation]  unity small-signal; loud peaks reduced (≈ 1/g); wet output ±12 dB
    ─► [Spatial]     L+R invariant (section 7)
    ─► [Compressor]  make-up −12..+24 dB, upward ≤ +18 dB (section 9)
    ─► [Maximizer]   drive 0..+24 dB → glue (only while armed) → soft clipper → TRUE-PEAK LIMITER @ ceiling
                     (−12..0 dBTP, default −1)
    ─► output gain −24..0 dB (trim, attenuation only)
    ─► global bypass crossfade (30 ms) with the dry reference (tapped after the fold, delayed by the chain latency):
                     × match gain (±12 dB, a raise capped at max.ceiling − held dry peak) → TRUE-PEAK LIMITER @ max.ceiling
                     (only while bypass is engaged; its latency is taken out of the dry delay, so none is added)
    ─► (desktop app only) Σ strips → master TP limiter −1 dBTP, 1 ms look-ahead (0.5 ms when every strip is Low Latency)
```

### 1.3 Where gain is added and removed

| Stage | Can add | Can remove | What bounds it | Source |
|---|---|---|---|---|
| Input gain (`input.gain`) | +24 dB | −24 dB | user; 20 ms linear ramp per block | `ProcessingChain.cpp` |
| AutoLevel (`autolevel.*`) | +12 dB | −12 dB | gated loudness loop on the input layout (BS.1770-4 channel weights, `analysis/ChannelWeights.h`), +1 dB/s up, −4 dB/s down (section 14) | `Protection.h` |
| Surround fold-down | — | 0.7071 × (sum of BS.775 contributions) | fixed, LFE dropped (virtualiser off); toggling `virt.on` (surround inputs only) crossfades virtualiser ↔ downmix over 20 ms (`virtMix`, `foldScratch`); switched on from fully off, the virtualiser starts from reset state | `ProcessingChain.cpp` |
| Parametric EQ | +24 dB per band, +12 dB output (store range) | −24 dB per band, −24 dB output, cuts | user only; no macro touches it | §2 |
| Dynamic EQ | `staticGain` + dynamic, `range` ≤ 24 dB | the same | `BoostBelow` fades out over the 10 dB above `noiseFloor`; mode bands scale with macros | §3 |
| Bass shelf | +15 dB (+ macros, clamped to 15) | never cuts | **predictive protection**: withdrawn by `softKnee(L + boost − protect)` | §4 |
| Bass harmonics | harmonics at up to ×2 (+6 dB) of the generated level; fundamental unchanged | optional replace-fundamental HP4 | level tracks the band linearly (envelope-normalised) | §4 |
| Clarity shaper | up to +12 dB on onsets (attack) / tails (sustain) | the same, as cuts | level-independent indicators; 1 ms gain smoothing | §5 |
| Presence / de-mud | +6 dB × presence | −4 dB × deMud | inverse-level computer with −80 dB RMS taper / relative threshold with −70 dB RMS gate | §5 |
| Air | 2nd/3rd harmonics at ≤ −12 dB re the 3.5–7 kHz band; +2 dB shelf at 10 kHz | — | envelope-normalised; disabled below 42 kHz | §5 |
| Saturation | wet make-up up to +12 dB (`sat.output`) | loud peaks (curve), −12 dB make-up | unity small-signal; see §6.3 for level behaviour | §6 |
| Maximizer | drive up to +24 dB (+ governed macros) | limiter / clipper / glue reduction | **true-peak ceiling**, SafetyGovernor, AutoDrive (reduce only); the glue splitter is only in the path while glue is armed (§1.4) | section 11 |
| Output gain (`output.gain`) | — (0 dB maximum) | −24 dB | trim applied **after** the maximizer, 20 ms ramp (see §1.4) | `ProcessingChain.cpp`, `Parameters.cpp` |
| Matched-bypass reference (`bypass.matched`, only while `bypass` is engaged) | up to +12 dB (LoudnessMatch) | −12 dB | a raise is capped once per block at `max.ceiling` − held dry peak and ramped over 50 ms; the reference then passes its own `TruePeakLimiter` at `max.ceiling` (`dryLimiter`), which catches a louder dry peak that arrives while the gain is still high (§14.5) | `ProcessingChain.cpp`, `Protection.h` |

### 1.4 Headroom policy

- **Headroom is float.** Nothing between the input stage and the maximizer clips; peaks above 0 dBFS inside the chain are legal. Protection acts on *predicted* or *measured* levels instead:
  - the bass protection predicts the boosted LF peak;
  - the governor measures the limiter's gain reduction (≤ 6 dB average) and the THD+N of the saturator and the soft clipper (≤ −30 dB, §14.5);
  - the limiter guarantees the ceiling.
- **The ceiling is a strip property.** The chain test *Chain: full Music boost on a hot programme never exceeds the ceiling* sets Boost Intensity and all five macros to 100 %, in both modes, on a hot programme. It asserts:
  - true peak ≤ −1 dBTP + 0.15 dB;
  - sample peak ≤ −1 dBFS;
  - zero safety-clamp engagements. This check is meaningful at chain level: the maximizer's limiter count (`LoudnessMaximizer::getSafetyClipCount()`) is published every block as `MeterBus::safetyClipCount`.
- **Factory presets hold it too.** *Factory presets: Boost Intensity and all macros at 100 % stay safe* (`tests/test_factory_presets.cpp`) renders every factory preset with every macro at 100 % and checks sample peak ≤ ceiling, true peak ≤ ceiling + 0.15 dB and no safety clamp. Because the limiter shares the meters' interpolator and holds its gain under the interpolation kernel (section 10), the margin is not needed in practice: measured for this document on the test's own programme, the 24 presets at their own settings and at full macros (48 renders) read at most −1.048 dBTP on the 4× meter for a −1 dBTP ceiling (−2.05 dBTP for the −2 dBTP Bluetooth preset), with no safety clamp. `LoudnessMaximizer.h` records the same result: worst −1.04 dBTP at full macros.
- **Output gain cannot break the guarantee.** `output.gain` is a trim of −24…0 dB applied after the limiter, so it can only lower the level below the ceiling.
- **Neither can the matched bypass.** The loudness-matched bypass raises the dry reference, which never passes the maximizer. Its per-block cap (`max.ceiling` − held dry peak) is not enough on its own: the gain ramps over 50 ms, so a new, louder dry peak can arrive while it is still high. Before the fix below, CLI renders of pink noise with 55 Hz kicks under the Loudness macro peaked at −0.13 to +0.28 dBFS against a −1 dBTP ceiling. The reference therefore passes a dedicated `TruePeakLimiter` at `max.ceiling` (true-peak detection, 80 ms auto release) that fits inside the chain latency the dry path is delayed by anyway (§14.6), so every host (app, plug-in, CLI) now gets the ceiling in bypass too. Test: *Chain: matched bypass never overshoots the ceiling when a louder dry peak arrives*.
  - Hosts that run a single `ProcessingChain` (plug-in, CLI) therefore need no extra safety net. With `--ceiling` or `--target-lufs` the CLI switches the maximizer on, or warns if `max.on=off` was requested explicitly (`tools/flubsound-cli/CliOptions.cpp`); it also warns if the measured true peak of a render exceeds the ceiling by more than 0.1 dB (`OfflineRenderer.cpp`).
  - In the desktop app several strips, each at its own ceiling, can sum above it. The `MixEngine` master limiter (−1 dBTP, 1 ms look-ahead or 0.5 ms when every strip runs Low Latency, 50 ms auto release, `MixEngine.cpp`) catches that; its safety-clamp count is exposed as `MixEngine::getMasterSafetyClipCount()`.
- **Glue is out of the path unless armed.** The maximizer's 3-band glue splitter is an all-pass, and a phase rotator raises the crest factor of flat-topped (mastered) material, which the limiter would then have to take back. `applyParameters()` therefore keeps the 0.001 glue floor only while glue is *armed* (`base[max.glue] > 0`, or a macro that can raise glue is above zero); otherwise glue is 0 and the splitter is out of the path. §11.3.2 has the rule, the measurements and the test (*Chain: with glue disarmed the maximizer passes hot flat-topped material untouched*: a −5 dBFS 100 Hz square passes the default maximizer unchanged within 1e−6 of the delayed input, with no gain reduction).
- **Known onset overshoots** are left for the limiter by design:
  - the bass protection's 10 ms detector attack lets a sudden loud bass note overshoot its cap by up to ≈ 3–4 dB, from roughly 12 ms to 40 ms after the onset (§4.3.3, §4.9);
  - the transient shaper can add up to +12 dB to onsets (§5).

### 1.5 Gaming vs Music

The rules are identical in both modes. The difference is *which* contributions are governed; see the tables in section 14:

- **Music:** bass boost, harmonics, maximizer drive and saturation drive (Boost Intensity, Loudness, Warmth).
- **Gaming:** bass boost, harmonics and maximizer drive (Boost Intensity, Impact). No Gaming macro engages or drives saturation (§6.8).

In both modes the tonal, spatial and detail contributions are ungoverned, because they add little loudness.

### 1.6 Tests that prove it

- *Chain: full Music boost on a hot programme never exceeds the ceiling*
- *Chain: matched bypass never overshoots the ceiling when a louder dry peak arrives*
- *Factory presets: Boost Intensity and all macros at 100 % stay safe*
- *Chain: with glue disarmed the maximizer passes hot flat-topped material untouched*
- *MacroMap: glue is armed only while a source that can raise it is off zero*
- *Chain: toggling the virtualiser on a 7.1 strip crossfades (no step in the output)*
- *ParameterStore: NaN is ignored, infinities clamp, and the chain stays finite*
- *Chain: everything bypassed = input delayed by the chain latency (bit-transparent path)*
- *Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz)*
- *SafetyGovernor: backs off under sustained over-limiting and recovers*
- *ParametricEq: all bands disabled is an exact null*
- *DynamicEq review: an active band that is not acting is an exact identity*
- *BassEngine (review): switching everything off lands on a bit-exact pass-through*
- *BassEngine: headroom protection withdraws the boost on loud low frequencies*
- *Clarity: neutral parameters are an exact pass-through*
- *TransientShaper: neutral settings are an exact pass-through*
- *Saturator: small-signal gain is unity (-40 dBFS, 1 kHz, drive 12 dB)*
- *Saturator: drive 0 dB is transparent and mix 0 is the exactly delayed dry signal*

---

## 2. Parametric EQ

Sources: [`core/include/flub/dsp/ParametricEq.h`](../core/include/flub/dsp/ParametricEq.h), [`core/src/dsp/ParametricEq.cpp`](../core/src/dsp/ParametricEq.cpp).

### 2.1 Purpose

The parametric EQ provides static tonal correction and voicing, for example headphone compensation, device tilt, or taste. It is a fully parametric, **minimum-phase, zero-latency** EQ with up to `kMaxBands = 16` bands; the chain exposes `kDefaultBands = 10`. It is allocation-free even in `prepare()`, and every parameter change is click-free.

### 2.2 Signal flow

```
x ─► band 0 ─► band 1 ─► … ─► band 9 ─► output gain (20 ms linear ramp) ─► y
        │
        └── per band:  y_b = x_b + mix · (H_b(x_b) − x_b)
                       H_b = cascade of 1..4 TPT SVF sections (same state machine for every band)
                       mix = wet amount: exactly 1 when settled, linear 0↔1 ramps for discrete changes
```

### 2.3 Algorithm & maths

**Band topologies** (`designBand()`). The same function designs the running filter and `responseDb()`, so the GUI curve is exactly what is heard:

| `eq.N.type` | SVF sections | Filter (§0.2) | Gain used | Q used |
|---|---|---|---|---|
| Bell | 1 | `Bell`, A² = 10^(gain/20) at f | yes | yes |
| Low Shelf | 1 | `LowShelf`, full gain below f, half at f | yes | yes |
| High Shelf | 1 | `HighShelf` | yes | yes |
| Low Cut | slope/12 = 1..4 | `HighPass` Butterworth (Q table §0.3) | no | no |
| High Cut | 1..4 | `LowPass` Butterworth | no | no |
| Notch | 1 | `Notch` (s² + 1)/(s² + s/Q + 1) | no | yes |
| Band Pass | 1 | `BandPass`, 0 dB at f | no | yes |

- Slope → sections: `N = (clamp(slope, 12, 48) + 6) / 12`, so 12/24/36/48 dB/oct give 1/2/3/4 sections. The chain maps the choice index i to `12·(i+1)`.
- Cuts are exact order-2N Butterworth filters: −3.01 dB at f, 6 dB/oct per order.
- Bands run in series, band 0 first, then the output gain. Each band's mix is `y = x + mix·(H(x) − x)`.

**Control rate and continuous glides.**
- Every `kControlInterval = 16` samples of stream time (0.33 ms at 48 kHz), each *busy* band runs a control tick.
- **Smoothing.** Three `OnePoleSmoother`s step at the control rate with a **20 ms** time constant: `log2(f)`, gain in dB and `log2(Q)`. The per-tick coefficient is `exp(−16 / (0.020·fs))`, i.e. the per-sample 20 ms coefficient raised to the 16th power, so the glide has the same time constant as a per-sample one-pole. Frequency therefore glides exponentially in log-frequency.
- **Redesign.** Coefficients are redesigned only when a smoothed value moved. Across the following 16 samples, the six coefficients (a1, a2, a3, m0, m1, m2) ramp linearly from the previous design to the new one (`runSectionRamped()`), so a sweep is a chain of short ramps with no 16-sample zipper.
- **Settling.** When a smoother stalls at float resolution short of its target, `stepSmoother()` lands it on the target. The final step is below 3e-4 dB or octaves, even at 192 kHz, and the ramp spreads it out. Once settled, the running coefficients use the exact target values, so they are **bit-identical** to what `responseDb()` designs. The shared `OnePoleSmoother` now snaps on its own too, so `stepSmoother()` is a second safeguard.

**Discrete changes** (enable, type, number of cut sections) cannot be interpolated, so they are crossfaded:

```mermaid
stateDiagram-v2
    [*] --> Settled
    Settled --> Settled: continuous change (glide, 20 ms)
    Settled --> FadingOut: discrete change while audible
    Settled --> Swap: discrete change while silent or skipped (0 dB)
    FadingOut --> Swap: wet mix reaches 0
    FadingOut --> FadingIn: change reverted
    Swap --> FadingIn: new topology, smoothers jump to targets, state cleared
    FadingIn --> Settled: wet mix reaches 1
```

- Each ramp lasts `fadeSamples = 16 · round(5 ms · fs / 16)`: 224 samples at 44.1 kHz, 240 at 48, 480 at 96 and 960 at 192 kHz.
- Because this is a whole number of control periods and the mix is computed from an integer position, the mix lands **exactly** on 0 and 1.
- While a swap is pending, the fading-out band keeps its *old* frequency, gain and Q. The new values arrive with the new topology under mix 0, so the old type never morphs towards values meant for the new one.
- A band that was not audible during the last control period (mix 0, or skipped as a 0 dB identity) swaps immediately and only fades in.

**CPU skipping and resume.**
- A band costs nothing while it is disabled and faded out.
- It also costs nothing while it is a Bell or shelf at exactly 0 dB that is not gliding, because it is then an exact identity (m0 = 1, m1 = m2 = 0).
- When a skipped 0 dB band starts to glide while audible, its state is primed: the SVF low-pass integrator `ic2` is set to the input sample (DC equilibrium) and `ic1` to 0. The output mix then ramps from identity to the new design over one control period, so leaving the identity path is step-free.
- When no band is busy, the rest of the block is processed as one segment. The output is sample-identical either way.

**State hygiene.**
- At the end of every processed segment, SVF states below 1e-15 are flushed to 0.
- A non-finite state restarts its section from rest, so a NaN input affects at most the current segment: one block while idle, at most 16 samples while busy.

**`responseDb(bands, n, f, fs)`** is static and pure, so it is safe on any thread:
- it sums `SvfCoeffs::magnitudeDb()` over the sections of every enabled band, after the same sanitising as the running filter;
- it ignores smoothing, crossfades and the output gain;
- it returns 0 for null or empty input, NaN frequency or a non-finite or non-positive sample rate.

### 2.4 Parameters

Band keys are `eq.N.*` with N = 0..9 (`param::kEqBands = 10`).

| Name | Key | Range | Default | Unit | Effect |
|---|---|---|---|---|---|
| Parametric EQ | `eq.on` | off/on | on | toggle | module bypass (`ModuleSlot`, 20 ms crossfade) |
| EQ Output | `eq.output` | −24 … +12 | 0 | dB | output gain, 20 ms linear ramp (the module itself accepts ±24) |
| Band N On | `eq.N.on` | off/on | on | toggle | enable (crossfaded) |
| Band N Type | `eq.N.type` | Bell, Low Shelf, High Shelf, Low Cut, High Cut, Notch, Band Pass | Bell | choice | topology (crossfaded) |
| Band N Frequency | `eq.N.freq` | 20 … 20 000 | 32, 64, 125, 250, 500, 1k, 2k, 4k, 8k, 16k | Hz | centre / corner, log glide 20 ms; the SVF clamps to 0.49 fs |
| Band N Gain | `eq.N.gain` | −24 … +24 | 0 | dB | Bell / shelves only |
| Band N Q | `eq.N.q` | 0.1 … 18 | 1.0 | — | Bell / shelves / Notch / Band Pass; ignored by the Butterworth cuts |
| Band N Slope | `eq.N.slope` | 12, 24, 36, 48 dB/oct | 24 dB/oct | choice | Low / High Cut only (crossfaded) |

With the defaults, all ten bands are enabled 0 dB bells, so all ten are skipped and cost nothing.

Module-level sanitising (`sanitise()`):
- frequency NaN → 1000 Hz, gain NaN → 0, Q NaN → 0.7071, ±inf clamps;
- an out-of-range type becomes Band Pass;
- `setBand()` with values identical to the current ones is free, and an invalid index is ignored.

### 2.5 Smoothing & click-freeness

| Change | Mechanism | Time |
|---|---|---|
| frequency / gain / Q | one-pole in log2 Hz / dB / log2 Q at control rate + per-sample coefficient ramp | 20 ms time constant; starts at the next 16-sample tick |
| enable / type / slope | wet-mix fade out → swap under mix 0 → fade in | 5 ms + 5 ms (whole control periods) |
| output gain | `LinearSmoothedValue`, applied sample-outer so every channel gets the identical ramp | 20 ms |
| module on/off | `ModuleSlot` equal-gain crossfade against the delayed dry path | 20 ms |

### 2.6 Latency & CPU

- **Latency: 0.** The EQ is minimum-phase IIR. Test *ParametricEq: zero latency - the impulse response starts at sample 0 and equals the raw SVF cascade* checks this.
- **CPU** (indicative; conditions under *Reading conventions*):

| Configuration | ns / stereo sample | % core |
|---|---|---|
| 10 static bells (±3 dB) | 97 | 0.47 % |
| 8 bells + two 48 dB/oct cuts | 162 | 0.78 % |
| 10 bands at 0 dB (defaults) | ≈ 0 (skipped) | ≈ 0 % |

Each SVF section costs a few ns per channel-sample. The recurrence is latency-bound, and channels are processed one after another. Control ticks cost O(bands) every 16 samples, and only while a band is busy. SIMD across channels is the next optimisation (**roadmap**, `docs/07-roadmap.md` 1.9).

### 2.7 Gaming vs Music usage

Neither the macros nor the mode policy touch the parametric EQ. `MacroMap` has no `eq.*` target, and `configureModeBands()` only drives dynamic-EQ bands. It is the user's or preset's static voicing layer and behaves identically in both modes. Mode-dependent, level-dependent tone shaping lives in the dynamic EQ (§3).

### 2.8 Tests that prove it (`tests/test_parametric_eq.cpp`)

- **Nulls:**
  - *ParametricEq: all bands disabled is an exact null*
  - *ParametricEq: bells and shelves at 0 dB are an exact null, also after toggles and glides*
- **Accuracy:**
  - *ParametricEq: measured sine gain matches responseDb() for every band type*
  - *ParametricEq: shape sanity - bell centre, shelf plateaus, notch depth, band-pass peak*
  - *ParametricEq: LowCut / HighCut are Butterworth (-3 dB at fc, 6 dB/oct per order)*
  - *ParametricEq: responseDb() sums enabled bands, ignores disabled ones, clamps and is finite*
  - *ParametricEq: 20 kHz bands are stable and accurate at 44.1 kHz and 192 kHz*
  - *ParametricEq: low-frequency bands stay accurate at 96 kHz and 192 kHz*
- **Click-freeness:**
  - *ParametricEq: abrupt gain / Q jumps are click-free (first-difference criterion)*
  - *ParametricEq: gain glides are as smooth as an ideal per-sample glide (no zipper, no stale state)*
  - *ParametricEq: abrupt type / slope / enable changes are crossfaded without clicks*
  - *ParametricEq: discrete changes use a linear ~5 ms wet/dry crossfade*
  - *ParametricEq: frequency glides in the log domain (~20 ms) without clicks*
  - *ParametricEq: the frequency glide is exponential in log2 (Hz), not in Hz*
  - *ParametricEq: output gain is smoothed, clamped and NaN-safe*
- **Robustness and RT safety:**
  - *ParametricEq: getBand() round-trips clamped values; bad indices are safe*
  - *ParametricEq: channels are independent and fewer channels than prepared work*
  - *ParametricEq: reset, setters and process are allocation-free*
  - *ParametricEq: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates*
  - *ParametricEq: a NaN / Inf input sample does not latch in the filter state*
  - *ParametricEq: output is independent of the host block size (1, 7, 64, 512)*
  - *ParametricEq: zero latency - the impulse response starts at sample 0 and equals the raw SVF cascade*
- **Review regressions:**
  - *ParametricEq (review): glides converge to the exact target design at every sample rate*
  - *ParametricEq (review): a type + gain change on a skipped 0 dB band never plays the old type*
  - *ParametricEq (review): a fading-out band keeps its old values until the swap*

### 2.9 Known limitations

- **No linear-phase mode.** It is roadmap, for offline/batch use only, because it would add latency.
- **Discrete changes crossfade through the dry signal.** Switching the type or slope of a steep cut on bright or bassy material briefly (5 ms each way) lets the unfiltered band through. This is an audible "flash", not a click. Running old and new filters in parallel during the fade would remove it; that is not implemented.
- **The SVF clamps design frequencies to 0.49 fs.** Below 40.8 kHz the top of the 20 kHz range is therefore clamped, and `responseDb()` reflects this consistently.
- **Changing a cut's Q or gain field makes the band "busy".** Q and gain do not affect the Butterworth cuts, so the band redesigns identical coefficients for the length of the glide. This costs CPU only.
- **Channel-count changes without `prepare()`.** If a host changes the channel count between blocks without calling `prepare()`, newly used channels resume from old state.

---

## 3. Dynamic EQ

Sources: [`core/include/flub/dsp/DynamicEq.h`](../core/include/flub/dsp/DynamicEq.h), [`core/src/dsp/DynamicEq.cpp`](../core/src/dsp/DynamicEq.cpp). Mode bands: `configureModeBands()` in [`core/src/engine/ProcessingChain.cpp`](../core/src/engine/ProcessingChain.cpp).

### 3.1 Purpose

The dynamic EQ is level-dependent EQ: each band's gain follows the level *in its own frequency region*. It handles:
- masking and resonance control (de-harsh, de-boom, explosion anti-masking);
- upward "detail" lifts, such as footsteps, voice and air, that only act on quiet content.

It has 8 bands (`kMaxBands`):
- bands 0–3 are the user's (`dyneq.N.*`);
- bands 4–7 belong to the Music/Gaming mode policy.

It has zero latency, and detection is **stereo-linked**, so a gain change never moves a source in the stereo image.

### 3.2 Signal flow

```
 x (module input, "dry") ──┬──► EQ band 0 ─► EQ band 1 ─► … ─► EQ band 7 ─► y        (bands in series, in place)
                           │        ▲             ▲                ▲
                           │        │ totalDb_b (glides over the next 16 samples)
                           │
                           └──► per active band b (sidechain, reads the DRY input):
                                  detector SVF (unity gain) ─► |y|, |inter-sample midpoint|
                                  ─► max over ALL channels and samples of the tick (linked peak)
                                  ─► 2-bucket sliding hold (W ticks) ─► env = max(held, env·e^(−1/W))
                                  ─► levelDb ─► gain computer (mode) ─► GainSmoother (attack/release)
                                  ─► totalDb = fade · (staticGain' + dynamic)
```

Every band's detector reads the module's input *before* any band has changed it. Each threshold therefore refers to the input level, and bands never chase each other.

### 3.3 Algorithm & maths

**Detector per shape.** Each detector is a unity-gain TPT SVF at the band's smoothed frequency and Q:

| Band shape | Detector | Detector Q | Hold time `T_hold` (clamped to [1 ms, 25 ms]) |
|---|---|---|---|
| Bell | `BandPass` (0 dB at f) | band Q | `2 / f_l`, where `f_l = f·(sqrt(1 + 1/(4Q²)) − 1/(2Q))` is the band-pass's lower −3 dB edge. This is half a period of f_l/4, where the skirt is 16 dB down at Q = 1. Example: 1 kHz, Q 1 gives 3.24 ms. |
| High Shelf | `HighPass` | min(Q, 0.7071) | `4 / f`: half a period of f/8, where the 12 dB/oct skirt is 36 dB down |
| Low Shelf | `LowPass` | min(Q, 0.7071) | always 25 ms, i.e. half a period of 20 Hz. The low-pass passes all bass at unity whatever f is. |

- The shelf detectors' Q is capped at Butterworth, so they never resonate above unity near the corner.
- Detector coefficients are redesigned at control rate while f or Q glides, without a ramp, because they only feed the level measurement.

**Inter-sample peak estimate.** A sample-peak detector reads a tone near fs/4 up to 3 dB low. A tone slightly off fs/4 makes that error beat slowly, so the gain tremolos. The detector therefore also evaluates the midpoint between earlier outputs with the maximally flat 6-point Lagrange half-sample interpolator:

```
mid = (150 (y3 + y2) − 25 (y4 + y1) + 3 (y5 + y0)) / 256          (midpoint between y3 and y2)
per-tick peak = max over channels and samples of max(|y0|, |mid|)
```

- The interpolator's gain is ≤ 1, so it never over-reads.
- The midpoint lags 2.5 samples, in the sidechain only.
- A 5-sample history per band and channel carries across blocks.
- Test *DynamicEq review: the level is not read low for treble near fs/4 (inter-sample peaks)* uses a CutAbove bell (ratio 4, ideal gain −22.5 dB). It asserts that the applied gain stays within [ideal − 0.03, ideal + 0.45] dB (never more cut than ideal) for tones at fs/4 and 100 Hz – 19 kHz, and ≤ 0.2 dB gain ripple for a tone detuned 5 Hz from fs/4.

**Level.** At every control tick (control rate `fs/16`):

```
W        = max(1, round(T_hold · fs/16))                    window in ticks
held     = max(windowPeak, prevWindowPeak)                  2 alternating buckets → covers W .. 2W ticks
env      = max(held, env · exp(−1/W))                       instant attack, decay τ = one window
env      = 0 if env < 1e-9
levelDb  = 20 log10(env)   (floor −160 dB)
```

The window always spans at least half a period of the lowest frequency that reaches the detector at a significant level. A steady tone therefore reads a ripple-free level, and the band does not intermodulate. Test *DynamicEq: a steady tone is not modulated by detector ripple (fast release, low frequency)* covers this.

**Gain computers** (dB), with `o = levelDb − threshold'`:

```
CutAbove   (downward compression): g = −min(range', max(0,  o) · (1 − 1/ratio'))
BoostBelow (upward compression)  : g = +min(range', max(0, −o) · (1 − 1/ratio')) · taper
                                   taper = clamp((levelDb − noiseFloor') / 10 dB, 0, 1)
BoostAbove (upward expansion)    : g = +min(range', max(0,  o) · (ratio' − 1))
CutBelow   (downward expansion)  : g = −min(range', max(0, −o) · (ratio' − 1))
```

Worked example (threshold −30 dBFS, ratio 2, range 6 dB, noise floor −70 dBFS):

| Detector level | −65 | −50 | −40 | −30 | −25 | −20 | −10 |
|---|---|---|---|---|---|---|---|
| CutAbove | 0 | 0 | 0 | 0 | −2.5 | −5 | −6 (capped) |
| BoostBelow | +3 (taper ½) | +6 (capped) | +5 | 0 | 0 | 0 | 0 |
| BoostAbove | 0 | 0 | 0 | 0 | +5 | +6 (capped) | +6 |
| CutBelow | −6 | −6 | −6 (capped) | 0 | 0 | 0 | 0 |

The noise-floor taper is what makes `BoostBelow` safe: silence and hiss are never lifted. On digital silence the level is −160 dB, so the taper is 0.

**Smoothing of the dynamic gain.**
- `dyn = GainSmoother(targetDb)` runs at control rate with the band's `attackMs` / `releaseMs` as one-pole time constants.
- `expanderMode` is true for `BoostAbove` and `CutBelow`, so "attack" is always the response to a *rising* detector level (§0.9).
- Once within 1e-5 dB, the gain snaps exactly onto its target.
- Time constants shorter than one control period (0.33 ms at 48 kHz) effectively act within a single tick.

**Total gain and the EQ section.**

```
totalDb = fade · (staticGain' + dyn)          published every tick (relaxed atomic) → getBandGainDb()
EQ      = SvfCoeffs::make(shape ∈ {Bell, LowShelf, HighShelf}, f', Q', totalDb)
```

- Coefficients are recomputed at a tick only when `totalDb` or the geometry changed.
- Across the next 16 samples the section glides linearly in (g, k, m0, m1, m2), re-deriving `a1 = 1/(1 + g(g + k))`, `a2 = g·a1`, `a3 = g·a2` per sample. Every intermediate filter is therefore a valid SVF.
- The ramp position comes from the global control phase.
- A glide between two 0 dB identity sets is skipped.

**Discrete changes** (enable, mode, shape) are click-free:
- A linear 0↔1 `fade` (`LinearSmoothedValue`, 20 ms at control rate: 60 ticks at 48 kHz) multiplies the band's total dB gain.
- At the bottom of the fade, with the EQ glide finished, the band is an exact identity. At 0 dB, Bell, LowShelf and HighShelf share one (g, k, m) set.
- There the band is either removed (disable), or re-typed (mode/shape swap). A swap:
  - switches the `GainSmoother` mode and resets it to 0 dB;
  - clears the level history and redesigns the detector;
  - keeps the SVF states;
  - then fades the band back in.
- Enabling an idle band starts it at its target settings with cleared state and a fade-in from 0.
- With no active band, `process()` only advances the control phase: zero cost and bit-transparent.

**Robustness.**
- When a block carries more channels than the previous one, the returning channels' detector state, EQ state and history are cleared, so a stale tail cannot ring out.
- NaN never enters the envelope: the running peak is the first argument of `std::max`.
- A non-finite band state is cleared within one control interval.

### 3.4 Parameters

**User bands 0–3** (`param::kDynEqBands = 4`, keys `dyneq.N.*`):

| Name | Key | Range | Default | Unit | Effect |
|---|---|---|---|---|---|
| Dynamic EQ | `dyneq.on` | off/on | on | toggle | module bypass (also engaged by macros, section 14) |
| Dyn N On | `dyneq.N.on` | off/on | off | toggle | enable (20 ms fade) |
| Dyn N Mode | `dyneq.N.mode` | Cut Above, Boost Below, Boost Above, Cut Below | Cut Above (N = 0–2), Boost Below (N = 3) | choice | gain computer (swap through 0 dB) |
| Dyn N Shape | `dyneq.N.shape` | Bell, Low Shelf, High Shelf | Bell (0–2), High Shelf (3) | choice | EQ + detector type |
| Dyn N Frequency | `dyneq.N.freq` | 20 … 20 000 | 90, 350, 3500, 10 000 | Hz | band / corner (20 ms log glide) |
| Dyn N Q | `dyneq.N.q` | 0.1 … 10 | 1.0 | — | EQ and detector Q (shelf detector capped at 0.7071) |
| Dyn N Threshold | `dyneq.N.threshold` | −80 … 0 | −24 | dBFS | detector peak level where action starts |
| Dyn N Ratio | `dyneq.N.ratio` | 1 … 20 | 2 | ratio | slope of the computer |
| Dyn N Range | `dyneq.N.range` | 0 … 24 | 6 | dB | maximum magnitude of the dynamic gain |
| Dyn N Static Gain | `dyneq.N.staticGain` | −12 … +12 | 0 | dB | fixed gain added to the dynamic gain |
| Dyn N Attack | `dyneq.N.attack` | 0.1 … 200 | 5 | ms | one-pole τ for a rising level |
| Dyn N Release | `dyneq.N.release` | 5 … 2000 | 80 | ms | one-pole τ for a falling level (plus the hold) |
| Dyn N Noise Floor | `dyneq.N.noiseFloor` | −100 … −30 | −70 | dBFS | `BoostBelow` lift fades out over the 10 dB above it (module accepts −120 … 0) |

- At module level, a non-finite value keeps the previous setting.
- An invalid mode becomes `CutAbove`, and a shape other than Bell or a shelf becomes Bell.
- Continuous values glide with a 20 ms one-pole at control rate: frequency and Q in the natural-log domain, plus threshold, ratio, range, static gain and noise floor.
- Attack and release times take effect immediately.

**Mode bands 4–7** are set every block by `configureModeBands()`. A band is enabled only while `range > 0.01 dB`, so it is idle and costs nothing when its macro is at 0:

| Mode | Band | Purpose | Mode / shape | f | Q | Thr (dBFS) | Ratio | Range | Att / Rel (ms) | Floor |
|---|---|---|---|---|---|---|---|---|---|---|
| Gaming | 4 | footstep detail | BoostBelow / Bell | 3200 Hz | 0.9 | −42 | 3 | 7 dB × Footsteps | 3 / 120 | −75 |
| Gaming | 5 | footstep body (heel) | BoostBelow / Bell | 260 Hz | 1.2 | −45 | 2.5 | 3 dB × Footsteps | 5 / 150 | −75 |
| Gaming | 6 | explosion anti-masking | CutAbove / Low Shelf | 90 Hz | 0.7 | −22 | 3 | 6 dB × Footsteps | 10 / 250 | −80 |
| Gaming | 7 | voice / score | BoostBelow / Bell | 2000 Hz | 0.7 | −36 | 2 | 4 dB × Voice & Score | 5 / 150 | −70 |
| Music | 4 | dynamic de-harsh | CutAbove / Bell | 3500 Hz | 1.2 | −22 | 3 | 3 dB × Clarity | 2 / 80 | −80 |
| Music | 5 | air lift | BoostBelow / High Shelf | 12 000 Hz | 0.7 | −45 | 2 | 3 dB × Clarity | 10 / 200 | −80 |
| Music | 6 | de-boom (paired with bass boost) | CutAbove / Bell | 120 Hz | 1.0 | −14 | 2.5 | 4 dB × Boost Intensity | 10 / 150 | −80 |
| Music | 7 | unused (range 0 → idle) | CutAbove / Bell | 1000 Hz | 1.0 | 0 | 1 | 0 | 5 / 80 | −80 |

The macro values used are the *effective* (post-MacroMap) values. The frequency column is a table in `ProcessingChain.cpp` (`kGamingModeBandHz`, `kMusicModeBandHz`); `ProcessingChain::modeBandFrequency(mode, band)` returns it for GUI markers (bands `kFirstModeBand = 4` … 4 + `kNumModeBands` − 1 = 7; 0 for any other band).

### 3.5 Smoothing & click-freeness

| Change | Mechanism |
|---|---|
| dynamic gain | `GainSmoother` (attack/release per mode convention) at control rate + per-sample (g, k, m) glide over 16 samples |
| frequency, Q, threshold, ratio, range, static gain, noise floor | 20 ms one-pole at control rate (log domain for f and Q) |
| enable / disable, mode / shape swap | 20 ms linear fade of the band's dB gain to 0 (an exact identity) → swap or remove → fade in |
| module on/off | `ModuleSlot` 20 ms crossfade |

Measured distortion from the gain control (implementer's measurements):
- a −6 dBFS 100 Hz note under an acting 1 kHz Bell (Q 1, default 5/80 ms) leaves intermodulation sidebands near −90 dBc;
- under a 1 kHz high shelf, about −113 dBc.

### 3.6 Latency & CPU

- **Latency: 0.** There is no look-ahead: the sidechain peak detection is instantaneous.
- **Per active band, per channel, per sample:** 2 SVF ticks (detector and EQ), the 3-multiply midpoint, and abs/max.
- **Per band, per tick:** one log10, the gain computer, and `SvfCoeffs::make` (tan, pow, sqrt) only when gain or geometry changed. A ramping band adds 16 divisions.

| Configuration (indicative) | ns / stereo sample | % core |
|---|---|---|
| 8 active bands | 212 | 1.0 % |
| all bands idle | ≈ 0 | ≈ 0 % |

### 3.7 Gaming vs Music usage

- **Gaming.**
  - The Footsteps macro scales bands 4–6: quiet 2–5 kHz "scuff and tick" and 150–400 Hz heel energy are lifted by upward compression, while loud events above threshold are untouched.
  - The anti-masking low shelf only acts on *very* loud low-frequency events and recovers with a 250 ms release, so the steps after an explosion are not buried.
  - Voice & Score scales band 7.
  - Linked detection keeps every source's direction intact.
- **Music.**
  - Clarity scales the de-harsh cut and the air lift that accompany the presence and air boosts of the clarity enhancer.
  - Boost Intensity scales the de-boom band that accompanies the bass boost.
- **Both.** Macros engage `dyneq.on`: Music via Boost Intensity and Clarity, Gaming via Footsteps and Voice & Score (section 14). The user bands 0–3 behave identically in both modes.

### 3.8 Tests that prove it (`tests/test_dynamic_eq.cpp`)

- **Gain computers:**
  - *DynamicEq: CutAbove at the band centre is range capped (-12 dB) and reported*
  - *DynamicEq: CutAbove with a 24 dB range follows the ratio (-15 dB)*
  - *DynamicEq: BoostBelow lifts quiet content, but never the noise floor*
  - *DynamicEq: BoostAbove and CutBelow (expansion) have the expected sign and slope*
  - *DynamicEq: static gain adds to the dynamic gain*
- **Detection:**
  - *DynamicEq: detection is stereo linked (a loud L drives the gain of a quiet R)*
  - *DynamicEq: a Bell band barely touches content two octaves away*
  - *DynamicEq: shelf detectors listen to their own side of the spectrum*
  - *DynamicEq: a steady tone is not modulated by detector ripple (fast release, low frequency)*
  - *DynamicEq: loud bass leaking through a detector skirt does not intermodulate*
  - *DynamicEq: detector reads the dry input, bands do not chase each other*
- **Timing:**
  - *DynamicEq: attack and release timing* (10 / 100 ms → 90 % in 18–30 ms and 200–265 ms)
  - *DynamicEq: level accuracy and timing do not depend on the sample rate*
  - *DynamicEq: expansion modes use attack for a RISING gain*
- **Click-freeness:**
  - *DynamicEq: disabled bands are bit-transparent; enable / disable glide click-free*
  - *DynamicEq: mode and shape switches are click-free crossfades*
  - *DynamicEq: continuous parameter changes glide*
- **RT safety and robustness:**
  - *DynamicEq: setBand sanitises parameters; index checks*
  - *DynamicEq: process, reset and setters do not allocate*
  - *DynamicEq: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates*
  - *DynamicEq: output is independent of the host block size*
  - *DynamicEq: zero latency - an impulse is not delayed*
- **Review regressions:**
  - *DynamicEq review: the level is not read low for treble near fs/4 (inter-sample peaks)*
  - *DynamicEq review: a channel that re-appears does not ring out stale filter state*
  - *DynamicEq review: bit-exact for any block split, with events at arbitrary samples*
  - *DynamicEq review: an active band that is not acting is an exact identity*
  - *DynamicEq review: hammering discrete changes stays click-free and settles on the last request*
  - *DynamicEq review: NaN / Inf input is flushed within two control intervals*
  - *DynamicEq review: re-preparing at another sample rate keeps levels and timing right*

### 3.9 Known limitations

- **No look-ahead**, by design, for zero latency. A fast transient passes at full level for roughly the attack time before a cut arrives.
- **The hold adds 1–2 windows before the release starts:**
  - 1–2 ms for treble bands;
  - about 3–6 ms for a 1 kHz Bell at Q 1;
  - 25–50 ms for low shelves.

  The envelope then decays with a time constant of one window, so a low-shelf band has an effective release floor of about 50 ms, whatever `releaseMs` is.
- **Minimum 1 ms hold.** Loud bass leaking through the skirt of a high-Q treble bell with a very low threshold can still modulate the gain slightly. Example: 3 kHz, Q 2, threshold −45 dBFS, −3 dBFS 100 Hz gives sidebands at −65 dBc with an 80 ms release and −47 dBc with 5 ms.
- **Residual level ripple** remains for tones very close to small rational fractions of fs: 0.13 dB near fs/4, 0.11 dB near fs/3, 0.04 dB near fs/6 (implementer's measurements).
- **`getBand()` returns a reference to audio-thread state.** GUIs must read band settings from the `ParameterStore`, not from `getBand()`. Post-macro values come from `ProcessingChain::effectiveValue(id)`, which the audio thread publishes once per block through relaxed atomics, and the live dynamic gain from `getBandGainDb()`.

---

## 4. Bass engine

Sources: [`core/include/flub/dsp/BassEngine.h`](../core/include/flub/dsp/BassEngine.h), [`core/src/dsp/BassEngine.cpp`](../core/src/dsp/BassEngine.cpp). The tighten stage embeds a `TransientShaper` (§5).

### 4.1 Purpose

The bass engine delivers "more bass" without the three usual costs: limiter pumping, muddy or phasey subs, and wasted headroom. It has five stages, all zero-latency IIR:
1. a subsonic high-pass;
2. mono bass;
3. a **headroom-protected** low shelf;
4. **psychoacoustic harmonics** (missing fundamental) for headsets and small speakers that cannot reproduce the fundamental;
5. a transient "tighten" of the low band.

### 4.2 Signal flow

```
 x_c (all channels, per sample)
  │
  ├─ 1. Subsonic HP4 (Butterworth, Q 1.3066 / 0.5412) @ subsonicHz            [parked stage, park 5 Hz]
  ├─ 2. Mono bass (exactly 2 channels): LR4 split @ monoBelowHz               [parked stage, park 10 Hz]
  │        out_L = (low_L + low_R)/2 + high_L,   out_R = (low_L + low_R)/2 + high_R
  │
  ├──────► protection detector (reads the pre-shelf signal):
  │          LP2 (Q 0.7071) @ max(150 Hz, 1.5·boostFrequency') → max_c|·| (linked)
  │          → 25 ms peak hold → EnvelopeFollower 10 ms / 150 ms → L (dBFS peak)
  │          control rate: withdraw = clamp(softKnee(L + boost' − protect'), 0, boost')
  │
  ├─ 3. Low shelf, Q 0.7 @ boostFrequency', gain = max(0, smooth_5ms(boost' − withdraw))
  │
  ├──────► 4. harmonics source: mid = mean_c(x_c)
  │          → HP2 25 Hz → LP4 @ cutoff' → peak hold 25 ms → env (0.5 ms / 50 ms)
  │          → xn = clamp(b/env, −1, 1) → env·(w2 T2 + w3 T3 + w4 T4 + w5 T5)(xn)
  │          → HP2 @ cutoff' → LP4 @ 6·cutoff' → × 2·amount (20 ms linear ramp) = h
  │
  ├─ replace fundamental (optional): HP4 @ cutoff' on every channel             [parked stage, park 10 Hz]
  ├─ x_c += h   (same harmonics added to every channel)
  ├─ 5. Tighten: LR4 @ 150 Hz; g = TransientShaper(attack 0, sustain = −12 dB·tighten')
  │        on max_c|low_c|;   out_c = g·low_c + high_c                          [parked stage, park 10 Hz]
  ▼
 y_c
```

### 4.3 Algorithm & maths

#### 4.3.1 Subsonic high-pass

- A 4th-order Butterworth HP (two SVF sections) at `subsonicHz` (10–40 Hz; 0 = off).
- At the default 20 Hz: 10 Hz is attenuated by 24.1 dB, 20 Hz by 3.01 dB, and 100 Hz by only 0.00001 dB.
- It removes DC and inaudible rumble that would otherwise consume limiter headroom and woofer excursion.

#### 4.3.2 Mono bass

- Active only when both the prepared spec and the block have **exactly 2 channels**; it is ignored for mono and surround.
- Each channel is split by an LR4 at `monoBelowHz` (40–250 Hz), and both channels get the mid of the lows: `out_c = (low_L + low_R)/2 + high_c`.
- For in-phase content the output is `AP(x)`: LP4 + HP4 = all-pass, so the magnitude is flat.
- Measured:
  - antiphase 50 Hz at a 120 Hz split: **−30.7 dB**;
  - antiphase 1 kHz: −0.002 dB.
- The result is a centred low end with no phasey, out-of-phase sub content.

#### 4.3.3 Adaptive low shelf with predictive headroom protection

- The shelf is an SVF `LowShelf` with Q 0.7 at `boostFrequency`, gain `G` dB.
- In the SVF convention, `boostFrequency` is the **half-gain corner** (§0.2).
- It is skipped (zero cost) while its gain is exactly 0 dB.

The protection works per sample for detection and per control tick for the gain:

```
detector      d[n]  = max_c | LP2_{fd}(x_c[n]) |,   fd = max(150 Hz, 1.5 · boostFrequency')
level         L     = 20 log10( EnvFollower_{10 ms att, 150 ms rel}( PeakHold_25ms(d) ) )
predicted     e     = L + boost' − protect'                       (dB over the cap, full boost assumed)
withdrawal    w     = clamp( softKnee(e), 0, boost' )
                      softKnee(e) = 0              for e <= −3
                                  = (e + 3)^2 / 12 for −3 < e < 3     (6 dB knee, C1-continuous)
                                  = e              for e >= 3
shelf gain    G     = max(0, smooth_5ms(boost' − w))              (net gain smoothed, never a cut)
telemetry     getProtectionDb() = clamp(smooth_5ms(w), 0, boost')
```

Why each piece is there:
- **Prediction, not feedback.** The detector reads the signal *before* the shelf and assumes the whole boost reaches it. This is conservative: content above the corner receives less than the full boost, so the real output lands at or a little below the cap.
- **Detector corner tracking.** A fixed 150 Hz detector under-read content near a 150–200 Hz shelf corner, which still receives about half the boost; the output overshot the cap by up to 2.6 dB. The detector LP therefore sits at `max(150 Hz, 1.5 × boostFrequency)` and is redesigned at control rate while `boostFrequency` glides. For shelves at or below 100 Hz nothing changes. The worst steady overshoot for any tone from 30 to 300 Hz and any shelf from 30 to 200 Hz is about +0.2 dB.
- **Net-gain smoothing.** The 5 ms smoothing is applied to the *net* shelf gain, so a boost change and the protection's reaction cannot pull the gain in opposite directions for a moment.
- **Telemetry.** It smooths the withdrawal itself. It is exactly 0 on quiet material while the boost is automated, instead of flashing the shelf's 5 ms lag as "protection".
- **Peak hold.** The 25 ms hold (§0.9) makes a steady bass note read as a constant, so the protection does not gain-modulate it.

Measured (48 kHz, cap 0 dBFS, boost 12 dB, 40 Hz sine at −6 dBFS, default subsonic):

| Shelf corner | Steady output peak | Protection (telemetry) | First 30 ms after a cold start |
|---|---|---|---|
| 70 Hz (default) | −0.63 dBFS | 5.94 dB | peaks +3.7 dBFS |
| 60 Hz | −1.05 dBFS | 5.94 dB | peaks +2.9 dBFS |

- The onset overshoot is the consequence of the specified 10 ms detector attack. Per half period (12.5 ms) of the 40 Hz tone, the output peaks read −1.1, +2.0, **+3.7**, +1.0, −1.0 dBFS (70 Hz corner), so the output is above the cap from roughly 12 ms to 40 ms after the onset. The downstream limiter absorbs it (§4.9).
- A quiet 40 Hz tone at −40 dBFS keeps the full designed boost, and the protection reads exactly 0.
- A tone already above the cap has the whole boost withdrawn and is never cut.

#### 4.3.4 Psychoacoustic harmonics ("missing fundamental")

**Rationale.** Small transducers, such as laptop speakers and compact headset drivers, cannot reproduce the lowest octaves at a useful level. When a harmonic series 2f₀, 3f₀, 4f₀ … is present, the auditory system still perceives the pitch f₀, and much of the "weight", even if f₀ itself is weak or absent: the residue or virtual pitch effect. So generating harmonics of the bass the device cannot reproduce, in the range it *can* reproduce, restores perceived bass without the excursion and headroom cost of boosting the fundamental.

**Why Chebyshev polynomials give exact harmonics.** Chebyshev polynomials satisfy `T_n(cos θ) = cos(nθ)`:

```
T2(x) = 2x^2 − 1          T3(x) = 4x^3 − 3x
T4(x) = 8x^4 − 8x^2 + 1   T5(x) = 16x^5 − 20x^3 + 5x
```

For a sinusoid `b = A cos θ` normalised by its own amplitude (`xn = b/env` with `env = A`), `xn = cos θ`. Each polynomial then produces **exactly** the n-th harmonic, with no other components and no DC:

```
y = env · Σ w_n T_n(xn) = A · (w2 cos 2θ + w3 cos 3θ + w4 cos 4θ + w5 cos 5θ)
```

The harmonic amplitudes scale *linearly* with A. Unlike a static waveshaper on the raw signal, the tone colour does not change with level, and the added energy is proportional to the bass content. Test *BassEngine: harmonics create clear 2nd and 3rd harmonics of a low tone* checks this linearity to 0.5 dB at −40 dB re the reference level.

**Implementation.**
- **Source.** The mid signal `mean_c(x_c)` after the shelf, band-limited by HP2 at 25 Hz and LP4 (Butterworth) at `harmonicsCutoff`.
- **Envelope.** 25 ms peak hold → `EnvelopeFollower` with 0.5 ms attack and 50 ms release. The hold keeps `env` equal to the tone's peak on steady notes, so the shaper sees a clean `cos θ`.
- **Shaper.** `xn = clamp(b/env, −1, 1)` (0 when env = 0), then `h = env·(w2T2 + w3T3 + w4T4 + w5T5)(xn)`.
- **Weights.** A linear morph with `harmonicsCharacter'` (20 ms smoothing):
  ```
  character 0 (even / warm):  (w2, w3, w4, w5) = (1, 0.35, 0.3, 0.1)
  character 1 (odd / punchy): (w2, w3, w4, w5) = (0.35, 1, 0.1, 0.3)
  ```
- **Post filter.** HP2 at `harmonicsCutoff` (12 dB/oct) and LP4 at `6 × harmonicsCutoff` (24 dB/oct). The steep upper slope keeps the output in the device's usable band and removes the splatter of the envelope's fast attack.
- **Mix.** Linear gain `2 × harmonicsAmount` (+6 dB at amount 1) with a 20 ms linear ramp. The **same** harmonics are added to every channel, so they are centred.
- **Idle path.** The harmonics path is skipped while the amount is 0 and restarts from clean state.
- **Telemetry.** Where the harmonics are added, per channel: the dry path (after the shelf and the optional replace-fundamental HP), the shaper's input `b` through the same post filter and mix (its linear branch, three more SVF sections per sample) and the added harmonics feed a two-reference estimate over analysis windows of at least 25 ms (§14.5). `getDistortionDb()` is the harmonics' share of the output there: for a 40 Hz tone −12.3 … −9.6 dB at amount 0.25 and −3.0 … −1.8 dB at amount 1 (by character), 0 … −0.8 dB with the fundamental replaced; on the drum-like test programme about −24 dB at Boost 50 % and −10 dB at Boost 100 % on most Music presets, −3.5 dB for Laptop Speakers at its own settings. It is not a governor input (§14.5).

Measured (48 kHz, 50 Hz tone, cutoff 120 Hz, amount 1). Levels are relative to the fundamental, which itself is unchanged (0.00 dB):

| Character | H2 (100 Hz) | H3 (150 Hz) | H4 (200 Hz) | H5 (250 Hz) |
|---|---|---|---|---|
| 0 (even) | +0.9 dB | −4.9 dB | −5.2 dB | −14.5 dB |
| 0.5 | −2.5 dB | +0.8 dB | −8.8 dB | −8.5 dB |
| 1 (odd) | −8.2 dB | +4.3 dB | −14.8 dB | −4.9 dB |

H2 sits below the 120 Hz cutoff and is attenuated by the post-HP, which is why the character morph moves it so much.

#### 4.3.5 Replace fundamental ("Small Speaker Mode") and tighten

- **Replace fundamental.** An HP4 Butterworth at `harmonicsCutoff` on every channel, applied *before* the harmonics are added. It removes the content the transducer cannot reproduce anyway, which reclaims headroom. Measured: 50 Hz with a 120 Hz cutoff, **−30.4 dB**.
- **Tighten.** An LR4 split at a fixed 150 Hz per channel. A `TransientShaper` (§5.3.1) with attack 0 and sustain `−12 dB × tighten` detects `max_c|low_c|` (linked). Its gain applies to the low band only: `out_c = g·low_c + high_c`. Negative sustain shortens the decay of bass notes ("punchy rather than boomy") and leaves content above the split alone. Test *BassEngine: tighten shortens low-frequency decays and leaves highs alone* checks this.

#### 4.3.6 Click-free switching: parked stages

Subsonic, mono, replace and tighten are *filters in the signal path* that switch in and out. Crossfading dry against an LR4-processed path directly would notch the corner region, because LP4 + HP4 is an all-pass at −180° there (§0.4). Every such stage is therefore a `ParkedStage`:

```
engage   : corner parked at 10 Hz (subsonic: 5 Hz, the SVF's lowest design frequency)
           → 20 ms linear crossfade dry → processed (the two differ only below the audible band)
           → only then does the corner glide to its target (25 ms one-pole in log frequency)
disengage: corner glides back to within 5 % of the park frequency (|ln f − ln f_park| < 0.05)
           (tighten also waits until its shaper gain is exactly 1)
           → 20 ms crossfade to dry → the stage stops (zero cost)
corner change on an engaged stage: plain log-frequency glide, per-sample g interpolation (GGlide)
```

The implementer measured a high-frequency (> 3 kHz) residual of at most about −87 dBFS while toggling.

### 4.4 Parameters

| Name | Key | Range | Default | Unit | Effect |
|---|---|---|---|---|---|
| Bass Engine | `bass.on` | off/on | on | toggle | module bypass |
| Bass Boost | `bass.boost` | 0 … 15 | 0 | dB | shelf gain before protection (20 ms smoothing) |
| Boost Frequency | `bass.freq` | 30 … 200 | 70 | Hz | shelf **half-gain corner** (25 ms log glide); also sets the detector corner max(150, 1.5·f) |
| Headroom Protect | `bass.protect` | −30 … 0 | −12 | dBFS | cap for the predicted boosted LF peak (20 ms smoothing) |
| Harmonic Bass | `bass.harmonics` | 0 … 1 | 0 | % | harmonics mix 0 … ×2 (+6 dB) |
| Speaker Low Limit | `bass.harmonicsCutoff` | 40 … 250 | 120 | Hz | harmonics band [cutoff, 6·cutoff]; replace-fundamental corner (25 ms log glide) |
| Harmonic Character | `bass.character` | 0 … 1 | 0.5 | % | even/warm ↔ odd/punchy weight morph |
| Small Speaker Mode | `bass.replaceFundamental` | off/on | off | toggle | HP4 at the cutoff on the original (parked stage) |
| Tighten | `bass.tighten` | 0 … 1 | 0 | % | low-band sustain 0 … −12 dB (parked stage below 150 Hz) |
| Mono Bass Below | `bass.monoBelow` | 0 … 250 | 0 (off) | Hz | LR4 mono-bass corner; values in (0, 40) clamp to 40 Hz |
| Subsonic Filter | `bass.subsonic` | 0 … 40 | 20 | Hz | HP4 corner; 0 = off, values in (0, 10) clamp to 10 Hz |

At module level NaN keeps the previous value and ±inf clamps. Unchanged parameters return early.

### 4.5 Smoothing & click-freeness

| Quantity | Smoothing |
|---|---|
| `boostDb`, `protectThresholdDb`, `harmonicsCharacter` | 20 ms one-pole at control rate |
| `boostFrequency`, `harmonicsCutoff`, parked-stage corners | 25 ms one-pole in log frequency at control rate; filters reach new designs by a per-sample g glide (`GGlide`) |
| net shelf gain | 5 ms one-pole at control rate; the shelf coefficients glide per sample in (g, k, m) (`SvfGlide`) |
| harmonics mix | 20 ms linear ramp per sample |
| tighten amount | the embedded shaper's 20 ms parameter smoothing and 1 ms gain smoothing |
| stage on/off | parked-stage sequence (§4.3.6); module bypass via `ModuleSlot` (20 ms) |

### 4.6 Latency & CPU

- **Latency: 0.** Test *BassEngine: zero latency - an impulse is not delayed* checks this.
- **CPU** (indicative):

| Configuration | ns / stereo sample | % core |
|---|---|---|
| defaults (only the 20 Hz subsonic HP active) | 24 | 0.12 % |
| all stages on (boost 9 dB, harmonics 0.5, replace, tighten 0.5, mono 120 Hz) | 165 | 0.79 % |

The harmonics telemetry (§4.3.4) is within run-to-run noise at this setting: 160–169 before, 162–171 ns after, in a same-session comparison.

### 4.7 Gaming vs Music usage

Macro contributions (section 14 has the full tables):

- **Music.**
  - **Boost Intensity:** boost +5 dB over 20–80 % and harmonics +0.30 over 35–90 %, both governed.
  - **Punch:** tighten +0.5 over 20–100 %.
  - **Warmth:** engages Bass; harmonics +0.20 over 40–100 % and boost +2 dB over 30–100 %, both governed.
  - Boost Intensity also scales the de-boom dynamic-EQ band (120 Hz, §3.4), which holds boomy passages in check while the shelf boosts.
- **Gaming.**
  - **Boost Intensity:** boost +3 dB over 30–90 %, governed.
  - **Impact** (explosions, gunshots): engages Bass; boost +6 dB (governed) and harmonics +0.25 over 40–100 % (governed).
  - The protection keeps explosions from turning the boost into limiter pumping.
  - The linked detector and the centred harmonics never shift a source's position.
- **Preset/user settings only** (no macro touches them): boost frequency, headroom protect, subsonic, mono bass, the harmonics cutoff and character, and Small Speaker Mode. These are device-oriented, e.g. raise *Speaker Low Limit* for a small headset driver.

### 4.8 Tests that prove it (`tests/test_bass_engine.cpp`)

- **Basics and each stage:**
  - *BassEngine: silence in gives exactly silence out; no DC*
  - *BassEngine: the low shelf boosts a quiet tone by its designed amount* (analytic shelf within 0.1 dB; +9 dB within 0.5 dB deep in the shelf)
  - *BassEngine: headroom protection withdraws the boost on loud low frequencies*
  - *BassEngine: harmonics create clear 2nd and 3rd harmonics of a low tone*
  - *BassEngine: harmonics character flips the 2nd / 3rd harmonic balance*
  - *BassEngine: replace-fundamental removes the original below the cutoff*
  - *BassEngine: mono bass cancels antiphase lows and leaves antiphase highs*
  - *BassEngine: subsonic filter removes rumble and keeps the bass*
  - *BassEngine: tighten shortens low-frequency decays and leaves highs alone*
- **Switching, RT safety and robustness:**
  - *BassEngine: switching features on and off is click-free*
  - *BassEngine: zero latency - an impulse is not delayed*
  - *BassEngine: process, reset and setters do not allocate*
  - *BassEngine: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates*
  - *BassEngine: output is independent of the host block size*
- **Review regressions:**
  - *BassEngine (review): protection telemetry does not flash while the boost is automated*
  - *BassEngine (review): the protection cap holds for every shelf frequency and tone*
  - *BassEngine (review): after loud material the output decays to exact silence without a reset*
  - *BassEngine (review): switching everything off lands on a bit-exact pass-through*
  - *BassEngine (review): every block size gives bit-identical output*
  - *BassEngine (review): steady bass through protection, tighten, mono and subsonic stays clean*
- **Harmonics telemetry** (`tests/test_distortion.cpp`, §14.5):
  - *Distortion: the bass harmonics generator's reading matches a harmonic analysis of the stage output within 0.05 dB (40 / 80 Hz, every character, with and without replacing the fundamental)* (measured < 0.001 dB)
  - *Distortion: linear settings of the bass engine and the clarity enhancer read -160 dB: harmonics / air off with every other stage engaged, after switching them off, and on silence*
  - *Distortion: the bass harmonics and air exciter readings do not depend on the host block size: a 55 Hz tone and a 4.4 kHz tone read the same in 32- and 4096-sample blocks* (within 0.2 dB of the 4096-sample reading; measured < 0.13 dB for the bass, < 0.002 dB for the exciter)
  - *Distortion: the monitor keeps the harmonic generators apart: power-summed and smoothed with tau = 300 ms, never in the THD+N block value or meter*
  - *Distortion: through the chain, the bass harmonics and the air exciter are not a governor input: harmonics far over the -30 dB budget leave the scale at exactly 1 and the THD+N meter at -160 dB, where a governor fed them would back off to its floor* (the governor policy of §14.5)
  - *Distortion: measuring in the bass harmonics generator and the air exciter, and the monitor's harmonics update, are allocation-free* (`tests/test_rtsan.cpp` checks the `FLUB_NONBLOCKING` annotations of `ParallelDistortion.h`, both readings and `DistortionMonitor::updateHarmonics`)

### 4.9 Reviewed design decisions & known limitations

- **`boostFrequency` is the shelf *corner*, i.e. the half-gain point** (SVF shelf convention, as in the parametric EQ). "9 dB @ 60 Hz" therefore gives about +7.3 dB at 40 Hz, +8.85 dB at 20 Hz and +4.5 dB at 60 Hz.
  - A Q 0.7 shelf reaches +8.85 dB at 40 Hz only with its corner at 120 Hz.
  - This was reviewed and kept, for consistency with every other shelf in the product. The test checks the analytic response, plus ≈ +9 dB deep in the shelf.
  - If "full boost at and below the set frequency" were wanted, the corner would have to sit about 1.5–2× higher, which changes the meaning of the control.
- **The protection detector corner tracks `max(150 Hz, 1.5 × boostFrequency)`**, not a fixed 150 Hz (see §4.3.3 for the overshoot it prevents).
- **Onset overshoot.** The 10 ms detector attack lets a sudden loud bass note exceed the cap by up to ≈ 3–4 dB, from roughly 12 ms to 40 ms after its onset (see §4.3.3). The maximizer's limiter absorbs this; it is not a ceiling risk.
- **Waveshaper clamp.** The shaper follows its envelope with a 0.5 ms attack and a hard clamp. When the band level rises faster than the envelope (note onsets, beating multi-tone bass), the clamp engages briefly and adds some intermodulation to the harmonics path. The 24 dB/oct upper limit keeps the residual above 3 kHz at about −55 dB re the programme (implementer's cutoff-sweep measurement).
- **Peak holds delay decay detection.** Holds delay the *start* of decay detection by about 25–33 ms: protection release, harmonics envelope and the tighten shaper. Attacks remain instant.
- **Mono bass requires exactly 2 channels.** If a 2-channel spec receives a 1-channel block, the mono stage pauses for that block, and it can resume with a small discontinuity. That only happens when a host changes the channel count mid-stream.
- **Disengaging a parked stage is deliberately gradual.** It takes about 100–150 ms: the glide to the park frequency, then the 20 ms crossfade.

---

## 5. Transient shaper + Clarity enhancer

Sources:
- [`core/include/flub/dsp/TransientShaper.h`](../core/include/flub/dsp/TransientShaper.h) and [`core/src/dsp/TransientShaper.cpp`](../core/src/dsp/TransientShaper.cpp) (the building block);
- [`core/include/flub/dsp/ClarityEnhancer.h`](../core/include/flub/dsp/ClarityEnhancer.h) and [`core/src/dsp/ClarityEnhancer.cpp`](../core/src/dsp/ClarityEnhancer.cpp) (the module).

### 5.1 Purpose

- **Clarity & detail.**
  - More punch and definition on onsets (drums, gunshots, footsteps).
  - Less smear from sustain.
  - Less low-mid "mud" when it dominates.
  - More intelligibility from a presence lift that backs off when the band is already loud.
  - "Air" from harmonics generated in the top octave.
- **All of it zero latency**, linked across channels where a gain is applied, so the stereo image is preserved.
- **`TransientShaper`** is a standalone building block. `ClarityEnhancer` uses it full-band; `BassEngine` uses it on the low band (§4.3.5).

### 5.2 Signal flow

```
 x_c
  ├─ 1. TransientShaper (linked): g[n] from d[n] = max_c|x_c[n]|, same g on every channel
  ├─ 2. De-mud:   bell 250 Hz, Q 1, gain = deMud' · G_mud              detectors on THIS stage's input:
  │                                                                     BP 250 Hz Q 1 and the broadband signal,
  │                                                                     linked mean squares (20 ms)
  ├─ 3. Presence: bell presenceFrequency', Q 0.8, gain = presence' · G_pres
  │                                                                     detector: BP f' Q 0.8, linked mean square (20 ms)
  ├─ 4. Air (per channel, not linked):
  │        h = HP4 3.5 kHz (x_c)          b = LP4 7 kHz (h)             (Butterworth)
  │        env = smooth_0.5ms( max( E(|b|), 0.7071 · E(|h|) ) )     E = 7.5 ms peak hold → 40 ms release
  │        xn = clamp(b / env, −1, 1);  y = env · (T2(xn) + 0.5·T3(xn))
  │        x_c += air' · 0.2512 · HP4 7 kHz (y)                          (−12 dB at air 1, 20 ms ramp)
  │     then high shelf 10 kHz, Q 0.7071, gain = 2 dB · air'
  ▼
 y_c
```

### 5.3 Algorithm & maths

#### 5.3.1 TransientShaper (level-independent)

```
d    = max_c |x_c|                      NaN → 0, clamped to 1e6
e    = PeakHold_25ms(d) + 1e-5          (window 25..33 ms; +1e-5 = −100 dBFS floor on every envelope)
attack pair : A_fast = Env(att 0.5 ms, rel 60 ms)(e),   A_slow = Env(att 20 ms, rel 60 ms)(e)
sustain pair: S_slow = Env(att 1 ms, rel 400 ms)(e),    S_fast = Env(att 1 ms, rel 40 ms)(e)
wA   = clamp( 20 log10(A_fast / A_slow) / 6 dB, 0, 1 )     > 0 at onsets
wS   = clamp( 20 log10(S_slow / S_fast) / 6 dB, 0, 1 )     > 0 in decays
gdB  = attackDb' · wA + sustainDb' · wS                    (' = 20 ms per-sample one-pole; each ±12 dB)
g    = 10^( smooth_1ms(gdB) / 20 )                         (symmetric 1 ms one-pole in dB, snaps within 1e-6 dB)
```

- **Level independence.** Only *ratios* of envelopes enter the gain, so the shaping follows the envelope's *shape*, not its level. The −100 dBFS floor keeps silence and fading hiss from reading as a "decay".
- **Why the hold.** Without the 25 ms peak hold, the 0.5 ms / 1 ms attack followers would re-acquire every waveform crest. The two envelopes of a pair would then disagree by a few dB at twice the note frequency, so the gain would amplitude-modulate sustained bass notes. With the hold, a steady tone reads as a constant, and both indicators are 0.
- **Neutral.** With attack and sustain at 0 dB and all smoothing landed, `computeGain()` returns **exactly 1.0f** and the log/exp work is skipped. The envelopes keep running so that re-enabling starts from a consistent state.
- **Cost per sample.** The logs are only evaluated while a ratio lies inside its 0–6 dB ramp: one max per channel, 4 one-poles, at most 2 logs and 1 exp.

#### 5.3.2 De-mud (dynamic cut at 250 Hz)

```
bandMs  = MS_20ms( max_c BP_{250 Hz, Q 1}(x_c)^2 )      broadMs = MS_20ms( max_c x_c^2 )
over    = dB(bandMs) − (dB(broadMs) − 12 dB)                           threshold tracks the broadband level
cut     = min(4 dB, softKnee6(over) · 0.5)                             6 dB soft knee (as §4.3.3), ratio 2:1
gate    = clamp((dB(broadMs) + 70) / 10, 0, 1)                         fades out between −60 and −70 dB RMS
G_mud   = GainSmoother_{15 ms att / 150 ms rel}( −cut · gate )           attack = cut deepening (gain falling)
EQ      : bell 250 Hz, Q 1, gain = deMud' · G_mud
```

Measured:
- a dominant 250 Hz tone gets **−4.0 dB** at deMud 1 and −2.0 dB at 0.5;
- a quiet 250 Hz component inside loud broadband material is untouched.

#### 5.3.3 Dynamic presence (inverse-level boost)

```
L       = dB( MS_20ms( max_c BP_{f', Q 0.8}(x_c)^2 ) )                 band level, dB RMS
boost   = clamp( (−18 − L) · 0.25, 0, 6 dB )                            full 6 dB at L <= −42 dB RMS, 0 at >= −18
taper   = clamp( (L + 80) / 10, 0, 1 )                                  no lift at/below −80 dB RMS (hiss)
G_pres  = GainSmoother_{5 ms att / 100 ms rel}( boost · taper )         attack = boost withdrawn (gain falling)
EQ      : bell f' (1–6 kHz), Q 0.8, gain = presence' · G_pres
```

For sines, RMS = peak − 3 dB. Measured at presence 1 on a 3.2 kHz tone (matches the formula):

| Tone level | Applied gain |
|---|---|
| −50 dBFS | +6.0 dB |
| −27 dBFS | +3.0 dB |
| −6 dBFS | 0 dB |

The result is intelligibility on quiet dialogue and quiet cues, without harshness on loud passages.

**Dynamic bells (de-mud and presence).**
- The gain computers run at control rate. The bells glide per sample in (g, k, m) (`SvfGlide`).
- A bell keeps running at 0 dB (an exact identity), so its state is current when the gain moves.
- A bell stops only once its amount target and value are 0 and it has landed on the identity.

#### 5.3.4 Air exciter and the alias-free argument

**Band limiting.**
- `h = HP4_3.5k(x)` is everything above 3.5 kHz; `b = LP4_7k(h)` is the 3.5–7 kHz band. Both are 24 dB/oct Butterworth.
- LP4 at 7 kHz reads −3.01 dB at 7 kHz, −5.1 dB at 7.6 kHz, −6.7 dB at 8 kHz, −11.3 dB at 9 kHz and −16.1 dB at 10 kHz (analytic, 44.1 kHz).

**Envelope.**

```
E(v)  = max( PeakHold_7.5ms(v), E_prev · c_40ms )      instant rise, 40 ms release (window 7.5..10 ms, ≥ 26 periods of 3.5 kHz)
env   = one-pole 0.5 ms of max( E(|b|), 0.7071 · E(|h|) )
```

**Shaper.**

```
xn = clamp(b / env, −1, 1)
y  = env · ( T2(xn) + 0.5 · T3(xn) ) = 2 b²/env − env + 2 b³/env² − 1.5 b      (for |b| <= env)
out = x + air' · 0.2512 · HP4_7k(y)          then high shelf 10 kHz, Q 0.7071, +2 dB · air'
```

**Why there is no oversampling.**
1. For a steady input, `env` is constant: the hold spans many periods of the band. `y` is then a polynomial of **order ≤ 3** in `b`.
2. The products of an order-3 polynomial occupy at most 3 × the input bandwidth: the spectrum of b² lies within [0, 14] kHz and that of b³ within [0, 21] kHz for `b` limited to 7 kHz.
3. So every product stays below 21 kHz, i.e. below Nyquist for any fs ≥ 42 kHz.
4. For a single tone at f the 3rd harmonic stays below Nyquist while f < fs/6: **7.35 kHz at 44.1 kHz**, 8 kHz at 48 kHz.
5. For a single sinusoid normalised to its own peak, `T2` and `T3` produce exactly the 2nd and 3rd harmonic (§4.3.4). For complex material the same polynomial also produces sum/difference and 2f₁ ± f₂ intermodulation terms, which are bounded by the same 21 kHz argument.
6. The final HP4 at 7 kHz keeps only the region above the band, and the 0.5 ms envelope smoother keeps level changes from splattering through the `−env` term.

**The skirt floor `0.7071·E(|h|)`.**
- The LP4 is not a brick wall. A tone at 7.6–9 kHz in its upper skirt reaches `b` attenuated, but normalised against its *own* small band level it would still be shaped at full depth, and its 3rd harmonic would fold back.
- The −3 dB floor derived from everything above 3.5 kHz, i.e. the LP4's level at its corner, makes such skirt content be shaped only gently.
- Content below 7 kHz is still normalised by the band envelope as before.
- The floor also backs the exciter off when the top octave is already bright.

Measured at air 1, levels re the input tone:

| Input | fs | 2nd harmonic | 3rd harmonic | Note |
|---|---|---|---|---|
| 4 kHz | 48 kHz | 8 kHz −13.8 dB | 12 kHz −17.8 dB | 16 kHz (4th) < −150 dB: order ≤ 3 |
| 6.5 kHz | 44.1 kHz | 13 kHz −12.3 dB | 19.5 kHz −18.0 dB | no alias |
| 7.6 kHz | 44.1 kHz | 15.2 kHz −17.1 dB | **alias at 21.3 kHz −25.0 dB** | skirt content |
| 8 kHz | 44.1 kHz | 16 kHz −20.1 dB | **alias at 20.1 kHz −29.8 dB** | |
| 8.5 kHz | 44.1 kHz | 17 kHz −24.8 dB | **alias at 18.6 kHz −36.7 dB** | |
| 9 kHz | 44.1 kHz | 18 kHz −30.0 dB | **alias at 17.1 kHz −44.3 dB** | |
| 8.5 / 9 kHz | 48 kHz | — | alias at 22.5 / 21 kHz: −36.0 / −43.0 dB | |

**Sample-rate guard.** `ProcessingChain` sets `air = 0` whenever **fs < 42 kHz**, for example USB 32 kHz modes and Bluetooth hands-free at 16 or 8 kHz, where the products would fold back. The guard is in `applyParameters()` (`if (fs < 42000) e[ClarityAir] = 0`, then `cp.air = e[ClarityAir]`) and matches the 21 kHz argument exactly: 3 × 7 kHz = 21 kHz < fs/2. Because it writes the effective value, `effectiveValue(clarity.air)` and the GUI's effective-value ring show the 0 that is applied (§14.1).

**Telemetry.** Where the harmonics are added (before the air shelf), per channel: the exciter's input, the band `b` through the same HP4 7 kHz and mix (the shaper's linear branch: below full scale `0.5·T3` leaves a term proportional to `b`, which is what the skirt floor produces; two more SVF sections per sample and channel) and the added signal feed a two-reference estimate over analysis windows of at least 25 ms (§14.5). `getDistortionDb()` is the generated harmonics' share of the output at that point: at air 1, −13.3 / −11.9 / −12.1 dB for 4 / 4.8 / 6 kHz tones and −35.1 dB for a 9.6 kHz skirt tone (a fit against the input alone read that one 12 dB high); −45 … −88 dB on the drum-like test programme. It is not a governor input (§14.5).

### 5.4 Parameters

| Name | Key | Range | Default | Unit | Effect |
|---|---|---|---|---|---|
| Clarity | `clarity.on` | off/on | on | toggle | module bypass |
| Transient Attack | `clarity.attack` | −12 … +12 | 0 | dB | onset emphasis (+) or softening (−) |
| Transient Sustain | `clarity.sustain` | −12 … +12 | 0 | dB | tail lengthening (+) or shortening (−) |
| Presence | `clarity.presence` | 0 … 1 | 0 | % | scales the dynamic presence boost (≤ +6 dB) |
| Presence Frequency | `clarity.presenceFreq` | 1000 … 6000 | 3200 | Hz | presence bell and detector centre (25 ms log glide) |
| Air | `clarity.air` | 0 … 1 | 0 | % | exciter mix 0 … −12 dB and 10 kHz shelf 0 … +2 dB (forced to 0 below 42 kHz fs) |
| De-Mud | `clarity.demud` | 0 … 1 | 0 | % | scales the 250 Hz dynamic cut (≤ −4 dB) |

At module level NaN keeps the previous value, other values are clamped, and unchanged parameters return early.

### 5.5 Smoothing & click-freeness

| Quantity | Smoothing |
|---|---|
| attack / sustain amounts | 20 ms one-pole per sample, inside the shaper; the gain itself has 1 ms dB smoothing |
| presence / de-mud amounts | 20 ms one-pole at control rate. When switched on, the stage starts from clean state with its amount rising from 0. |
| dynamic bell gains | `GainSmoother` at control rate (15/150 ms de-mud, 5/100 ms presence), then a per-sample `SvfGlide` |
| presence frequency | 25 ms one-pole in log frequency at control rate; the detector is redesigned per tick, the bell glides |
| air | exciter mix: 20 ms linear ramp per sample. Shelf: amount smoothed 20 ms at control rate, then glided. When switched on, all states start clean. |
| switching off | a stage runs until its amount and EQ gain are exactly 0, then stops |
| module on/off | `ModuleSlot` 20 ms crossfade |

Test *TransientShaper: parameter changes and onsets move the gain smoothly* bounds the gain's slew at 48 kHz: a full 24 dB swing moves at most 0.55 dB per sample.

### 5.6 Latency & CPU

- **Latency: 0.** Tests: *TransientShaper: zero latency - an impulse is not delayed* and *Clarity: zero latency - an impulse is not delayed*.
- **CPU** (indicative):

| Configuration | ns / stereo sample | % core |
|---|---|---|
| neutral (only the shaper's detector runs) | 9.6 | 0.05 % |
| all stages on (attack +6, sustain −3, presence 1, air 1, de-mud 1) | 151 | 0.73 % |

The exciter telemetry (§5.3.4) adds about 20 ns at this setting: 124–130 before, 147–149 ns after, in a same-session comparison (most of it the two extra SVF sections of the linear branch).

### 5.7 Gaming vs Music usage

- **Music.**
  - **Boost Intensity:** presence +0.35 (0–50 %), air +0.30 (10–60 %), attack +2 dB (10–60 %).
  - **Punch:** attack +6 dB.
  - **Clarity:** presence +0.8, air +0.7 (20–100 %), de-mud +0.5 (0–70 %). It also drives the dynamic-EQ de-harsh (3.5 kHz) and air-lift (12 kHz) bands, so the static-looking brightness is dynamically policed.
- **Gaming.**
  - **Boost Intensity:** presence +0.3 (0–50 %), attack +2 dB (20–70 %).
  - **Impact:** attack +4 dB (20–100 %).
  - **Detail:** air +0.4 (20–100 %).
  - **Voice & Score:** presence +0.7, de-mud +0.4 (20–100 %).
- **Gaming-relevant properties.**
  - The shaper, de-mud and presence gains are linked, so they never move a source.
  - The air exciter runs per channel. Its harmonics scale linearly with each channel's own band level, so a panned source keeps its interaural level difference in the generated harmonics.
- **Engagement.** Macros engage `clarity.on` where listed in section 14.

### 5.8 Tests that prove it

`tests/test_transient_shaper.cpp`:
- *TransientShaper: neutral settings are an exact pass-through*
- *TransientShaper: +12 dB attack lifts the onset of noise bursts relative to their sustained part* (≥ +6 dB onset lift, steady noise within 1 dB)
- *TransientShaper: negative sustain shortens a decaying tone's tail, positive sustain lengthens it* (tail ≥ 6 dB lower or higher)
- *TransientShaper: the shaping is independent of the absolute level* (≤ 0.5 dB difference)
- *TransientShaper: detection is linked - every channel gets the same gain*
- *TransientShaper: parameter changes and onsets move the gain smoothly*
- *TransientShaper: zero latency - an impulse is not delayed*
- *TransientShaper: process, reset and setters do not allocate*
- *TransientShaper: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates*
- *TransientShaper: output is independent of the host block size*
- *TransientShaper (review): steady low notes are not gain-modulated at any rate*
- *TransientShaper (review): gain is bounded and slews smoothly at every sample rate*
- *TransientShaper (review): neutral is bit-exact again after garbage input at every rate*

`tests/test_clarity.cpp`:
- *Clarity: neutral parameters are an exact pass-through*
- *Clarity: transient attack lifts onsets (linked, full band)*
- *Clarity: de-mud reduces a dominant 250 Hz tone and ignores balanced material* (−4.0 ± 0.5 dB at 1, −2.0 ± 0.5 dB at 0.5)
- *Clarity: dynamic presence lifts quiet content and leaves loud content*
- *Clarity: air creates 2nd and 3rd harmonics of upper-mid content* (no 4th harmonic: ≤ −100 dB)
- *Clarity: air produces no aliasing at 44.1 kHz* (6.5 kHz tone: worst non-harmonic component ≤ −80 dB; skirt tones at −12 dBFS: the 9 kHz 3rd-harmonic alias ≤ −40 dB, the 10 kHz 3rd-harmonic and 12 kHz 2nd-harmonic aliases ≤ −50 dB re the tone level)
- *Clarity: parameter changes are click-free*
- *Clarity: zero latency - an impulse is not delayed*
- *Clarity: process, reset and setters do not allocate*
- *Clarity: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates*
- *Clarity: output is independent of the host block size*
- *Clarity (review): after loud material the output decays to exact silence without a reset*
- *Clarity (review): every block size gives bit-identical output*
- *Clarity (review): steady tones through shaper, de-mud and presence stay free of modulation products*
- *Clarity (review): presence and de-mud behave the same at every sample rate*

`tests/test_distortion.cpp` (exciter telemetry, §5.3.4 and §14.5):
- *Distortion: the air exciter's reading matches a harmonic analysis of the stage output (the linear air shelf taken out) within 0.05 dB, also on the band's skirt* (measured < 0.001 dB)
- *Distortion: the parallel-generator estimator counts only what neither the dry path nor the generator's linear branch explains: it matches a harmonic analysis where the dry-path fit alone reads high, and falls back to it when the branch adds no direction*
- the −160 dB, block-size, monitor, chain-policy and allocation tests, shared with the bass harmonics, are listed in §4.8

Chain level: *Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz)* runs all macros at 100 % at every rate, including the ones where air is forced off. *Headset: below 42 kHz (hands-free 8 / 16 kHz, USB 32 kHz) the air exciter is cut off - nothing is added above the input band; at 44.1 / 48 kHz the same setting adds its harmonics and shelf* (`tests/test_protection_gaps.cpp`) asserts the guard itself: with only Clarity on and air 100 %, the effective air reads 0 (also when the Clarity macro raises it) and the output at 8, 16 and 32 kHz is the delayed input within 1e−6; at 44.1 and 48 kHz the 2nd harmonic (≥ −30 dB) and the 10 kHz shelf (≥ +1 dB at 12 kHz) are there.

### 5.9 Reviewed design decisions & known limitations

- **Air exciter aliasing at 44.1/48 kHz.** The exciter is alias-free for content ≤ ~7.35 kHz at 44.1 kHz (≤ 8 kHz at 48 kHz).
  - Strong *tonal* content at 7.6–9 kHz, in the LP4 skirt, still leaves small aliases of −25 to −44 dB re the tone at air 1 (table in §5.3.4).
  - These fold to 17–21 kHz, where hearing is least sensitive and where the much louder exciting tone and its harmonics partly mask them.
  - The chain disables air below 42 kHz fs.
  - A frequency-selective envelope floor or 2× oversampling of the shaper would remove the aliases. Neither is implemented.
- **De-mud trims balanced (pink-like) material.**
  - The threshold is *broadband − 12 dB*. A Q 1 band at 250 Hz on pink noise sits only ≈ 8–9 dB below the broadband level (8.8 dB measured with the module's own detectors on Kellet-filtered pink noise at 48 kHz; 7.6 dB analytically for ideal 20 Hz – 20 kHz pink noise), i.e. ≈ 3–4 dB above the threshold. The level fluctuations of noise, the convex soft knee and the faster attack (cut deepening) push the average cut a little beyond the steady-state value of half that excess.
  - At de-mud 1 this trims pink-noise-like material by **≈ 1.7–2 dB at 250 Hz**: about 1.7 dB in the review measurement, and 1.9–2.0 dB in this document's harness with Kellet-filtered pink noise.
  - The unit test's "balanced material" case uses white noise, which is treble-heavy and does not show this.
  - This is a **tuning item**: a threshold of about broadband − 6…8 dB, or a milder ratio, would leave pink-ish spectra alone.
- **Absolute level thresholds.** The presence thresholds (−18 / −42 dB RMS, floor −80 dB RMS) and the de-mud gate (−70 dB RMS) are absolute, so they assume the chain's nominal level. AutoLevel, when enabled, keeps the input near its target.
- **Residual attack gain after a click.** Both envelopes of the attack pair release with the same 60 ms time constant, so the onset ratio A_fast/A_slow built up by an isolated click decays only slowly. Quiet material that follows within about 100–200 ms can receive a few dB of residual attack gain. The peak hold reduces this but does not remove it; it is inherent to the specified topology.
- **Peak holds delay decay detection.** They delay the start of decay detection by 25–33 ms in the shaper and 7.5–10 ms in the air envelopes. Attacks stay instant.
- **Float envelope precision at high sample rates.** At 96/192 kHz the float followers settle within about 1e-4 of their input, so the steady-state shaper gain on constant material may sit up to about 0.002 dB off unity (implementer's measurement).

---

## 6. Saturation

Sources: [`core/include/flub/dsp/Saturator.h`](../core/include/flub/dsp/Saturator.h), [`core/src/dsp/Saturator.cpp`](../core/src/dsp/Saturator.cpp); oversampling §0.6.

### 6.1 Purpose

Saturation adds harmonic warmth and density: tape, tube or digital character. Two properties hold for all three types:
- **unity small-signal gain**, so drive changes the *character* and the peak behaviour, not the loudness of quiet material;
- **delta oversampling** (§0.6), so the generated harmonics do not alias while the programme itself never passes the half-band filters: no top-octave droop.

The dry/wet mix is latency-aligned internally, so any mix is comb-free.

### 6.2 Signal flow

```
x ──┬──► Oversampler ↑ (factor F) ─► x̂ ─► curve(type) at F·fs ─► f(x̂) − x̂ ─► Oversampler ↓ ──► d
    │                                     (Tape: pre-emphasis → tanh → de-emphasis)            │
    │                                                                                          │
    └──► DelayLine (L = oversampler round trip) ─────────────────────────────► x_d ──────────(+)──► s = x_d + d

base-rate post-processing (per sample):
    s'   = s − tubeW · LP10(s)                  tube DC blocker: s − LP10(s) = 1st-order TPT HP at 10 Hz
    s''  = s' + bumpBeta · tapeW · BP80(s')      tape head bump: peaking bell of exactly (1 + bumpBeta) at 80 Hz
    core = x_d + depth · (s'' − x_d)            drive depth (0 at 0 dB drive, 1 from 6 dB)
    y    = x_d + mix · (outGain · core − x_d)    latency-aligned dry/wet; outGain acts on the wet path only
```

`tubeW` and `tapeW` are 1 for the active type and 0 otherwise; they ramp during a type crossfade.

**Delta oversampling.** The upsampled input `x̂` is kept (`osInput`), and only the deviation `f(x̂) − x̂` is downsampled; during a type crossfade it is `(1 − w)·f_from(x̂) + w·f_to(x̂) − x̂`. The programme itself reaches `s` through the exact dry delay. The latency is unchanged, and the aliasing is unchanged too, because the harmonics still pass the same downsampler (§6.4).

### 6.3 Algorithm & maths

**Curves.** With `g = 10^(drive/20)`, every curve satisfies `f(0) = 0` and `f'(0) = 1`, so `y = f(g·x)/g ≈ x` for small x.

| Type | Curve (oversampled) | Harmonics | Notes |
|---|---|---|---|
| Tape | `de( tanh( g · pre(x) ) / g )` | odd only (tanh is odd). The pre-emphasis makes HF saturate earlier. | `pre` = +6 dB / `de` = −6 dB SVF high shelves at 3 kHz, Q 0.7071, designed at F·fs. They have the same f and Q with negated gain, so they are **exact inverses** (analytic cascade deviation < 1e-14 dB); the small-signal path is flat. Plus the head bump at base rate: `bumpDb = 1 dB · drive / 24`. |
| Tube | `f(u) = (tanh(u + b) − tanh b) / (1 − tanh² b)`, b = 0.2 | even + odd (asymmetric) | Evaluated as the identical closed form `tanh(u) / (1 + tanh(0.2)·tanh(u))`, which follows from the tanh addition theorem and avoids float cancellation at low levels. Asymptotes +0.835 / −1.246. Generated DC is removed by the 10 Hz blocker. |
| Digital | `f(u) = u − (4/27) u³` for \|u\| < 1.5, `sign(u)` beyond | odd only | f(1.5) = 1 and f'(1.5) = 0, so the join is C1: a soft clip with a hard limit at ±1 |

`Saturator::shape(type, x)` returns the bare static curve `f(x)` (tanh for Tape) for tests and the GUI transfer plot. Out-of-range enum values are treated as Digital.

**Drive depth.**
- With `f(g·x)/g` alone, 0 dB drive would still be `tanh(x)`: several % THD and −2.4 dB peaks on full-scale material.
- Engaging the module, for example through the Warmth macro's drive ramp from 0 dB, would then jump in character. The curve is therefore blended in:
  ```
  depth = smoothstep(0, 6 dB, drive) = 3t² − 2t³,   t = drive / 6 dB
  ```
- At 0 dB drive the output is **exactly** the delayed input. From 6 dB up it is exactly `f(g·x)/g`.
- Both terms have unity small-signal gain, so the blend keeps that property.

**DC blocker and head bump** (base rate):
- **DC blocker.** A TPT one-pole: `G = g_dc/(1 + g_dc)`, `g_dc = tan(π·10/fs)`, `v = (s − z)·G`, `lp = v + z`, `z = lp + v`, `HP = s − lp`. It runs continuously, so it is already settled when a crossfade brings Tube in.
- **Head bump.** A unity-peak SVF band-pass at 80 Hz, Q 1.0, so `s + β·BP80(s)` is a peaking bell of exactly `1 + β` at 80 Hz, with `β = 10^(bumpDb/20) − 1`. It is a constant-Q peak with no dip below it, a simplification of a real tape head bump.

**Measured THD** (1 kHz, −6 dBFS, 48 kHz, 2× High; harmonics H2–H20; the fundamental change shows the level behaviour):

| Drive | Tape THD / fund. | Tube THD / H2 / fund. | Digital THD / fund. |
|---|---|---|---|
| 1 dB | 0.12 % / −0.05 dB | 0.37 % / −49 dBc / −0.04 dB | 0.09 % / −0.02 dB |
| 6 dB | 4.8 % / −1.8 dB | 9.4 % / −23 dBc / −1.7 dB | 4.1 % / −1.0 dB |
| 12 dB | 12.3 % / −5.1 dB | 18.3 % / −23 dBc / −4.7 dB | 19.6 % / −4.4 dB |
| 24 dB | 25.5 % / −16.0 dB | 38.7 % / −32 dBc / −15.6 dB | 40.7 % / −15.9 dB |

Tape and Digital show no even harmonics: H2 is below −170 dBc.

**Level behaviour ("auto gain").** There is **no automatic gain-compensation loop** in the saturator. Level neutrality comes from the construction:

- **Quiet material** passes at unity for any drive. Pink noise at −30 dBFS RMS changes by ≤ 0.21 dB through Tape or Digital even at 13 dB drive. Tube reads about 1 dB lower on the same noise at any drive: its always-on 10 Hz DC blocker also removes the infrasonic part of the Kellet pink noise, which is a filter effect, not a level-dependent one.
- **The top octave is flat too.** Because of delta oversampling, a quiet tone never passes the half-band filters. Measured for this document at drive 12 dB, −40 dBFS: tones at 1, 18, 20 and 21 kHz read within 0.015 dB of unity for every type, at 44.1 and 48 kHz, with 2× Low and 2× High. Test *Transparency: the saturator keeps a quiet 19.5 kHz tone at unity gain with drive engaged* asserts ±0.1 dB at 19.5 kHz for the same cases.
- **Loud material loses level.** The wet path's peak ceiling is about `max|f| / g`, i.e. about 1/g for Tape and Digital. Measured on pink noise at −12 dBFS RMS with peaks near 0 dBFS, at 2× Low:

  | Type | Drive 4 dB | Drive 9 dB | Drive 13 dB |
  |---|---|---|---|
  | Tape | −1.2 dB RMS | −3.6 dB RMS (peaks −8.8 dBFS) | −6.1 dB RMS |
  | Digital | −0.5 dB RMS | −1.9 dB RMS | −4.0 dB RMS |

- **Make-up is manual.** `sat.output` (±12 dB) is a wet-path make-up gain. In the macro-driven workflow, loudness is restored downstream by the maximizer (Loudness macro, Boost Intensity) and bounded by AutoDrive. The loudness-matched global bypass (section 14) keeps A/B comparisons fair.
- An automatic wet make-up based on measured loudness is **not implemented**.

**Distortion telemetry.** `getDistortionDb()` is the stage's THD+N over the last completed analysis window (at least 25 ms, §14.5), measured with the least-squares estimator of `DistortionEstimator.h` (§14.5) *around the curve*, at the oversampled rate, where the upsampled input `x̂` and the curve's deviation `f(x̂) − x̂` are aligned (the round-trip latency never enters). Depth, mix and output gain are folded in: the output is `a·x + b·(f − x)` with `a = 1 − mix + mix·outGain` and `b = mix·outGain·depth`, so the deviation is weighted by `b/a` (held per base-rate sample). The linear post filters (Tube DC blocker, Tape head bump) are not part of it; the DC the Tube bias creates is counted, although the 10 Hz blocker removes it afterwards. The reading includes harmonics above the base-rate Nyquist that the downsampler removes. At 0 dB drive or mix 0 it is −160 dB. Test *Distortion: the saturator's and the soft clipper's in-stage readings match a harmonic analysis of what the stages do within 0.1 dB* compares it with a Goertzel analysis of the saturator's actual output (Digital and Tape, 6–15 dB drive; measured difference < 0.01 dB).

### 6.4 Oversampling and aliasing

| Profile (`latency.profile`) | Saturator oversampling | Latency |
|---|---|---|
| Quality | 2× High | 32 samples (0.67 ms at 48 kHz) |
| Balanced (default) | 2× Low | 16 samples (0.33 ms) |
| Low Latency | 2× Low | 16 samples (0.33 ms) |
| API (`setOversampling()` before `prepare()`; constructor default 2× High) | 1×, 2× or 4×, High or Low. Factors other than 1/2/4 are sanitised in `prepare()`: ≥ 4 → 4, ≥ 2 → 2, else 1. | 0 / 32 / 36 (High), 16 / 19 (Low) |

Torture test: a 15 kHz sine at −6 dBFS, drive 12 dB, 48 kHz. The output is periodic in 16 samples, so every in-band component other than 15 kHz is an alias. The table gives the worst alias re the tone (re-measured on the delta-oversampled saturator):

| Type | 1× | 2× Low | 2× High | 4× Low | 4× High |
|---|---|---|---|---|---|
| Tape | −8.4 dB | −19.3 dB | −19.0 dB | −42.3 dB | −43.0 dB |
| Tube | −15.8 dB | −29.8 dB | −29.4 dB | −62.2 dB | −74.2 dB |
| Digital | −14.3 dB | −32.2 dB | −31.8 dB | −59.4 dB | −77.0 dB |

- The Tape figures are the worst because its +6 dB pre-emphasis drives a 15 kHz tone 6 dB harder into the tanh.
- At 2× the dominant alias is the 5th harmonic (75 kHz) folding to 21 kHz.
- Programme material has far less energy at 15 kHz than this test. Heavy saturation of loud HF content still aliases at the chain's 2× setting.
- **Antiderivative anti-aliasing (ADAA) is roadmap** (`docs/08-pitfalls-and-solutions.md` B3).

### 6.5 Parameters

| Name | Key | Range | Default | Unit | Effect |
|---|---|---|---|---|---|
| Saturation | `sat.on` | off/on | **off** | toggle | module bypass (macros can engage it) |
| Saturation Type | `sat.type` | Tape, Tube, Digital | Tape | choice | curve (20 ms crossfade) |
| Drive | `sat.drive` | 0 … 24 | 0 | dB | curve input gain g (1/g after); depth blend below 6 dB; tape head bump 0…+1 dB |
| Mix | `sat.mix` | 0 … 1 | 1 (100 %) | % | latency-aligned dry/wet (mix 0 = exactly the delayed dry signal) |
| Saturation Output | `sat.output` | −12 … +12 | 0 | dB | wet-path make-up gain |

At module level NaN falls back to the default, values are clamped, and an unchanged parameter set returns early.

### 6.6 Smoothing & click-freeness

- **Drive, mix and output** use 20 ms linear ramps (`LinearSmoothedValue`), stepped once per base-rate sample and shared by all channels.
  - While drive ramps, the curve gain `g` is also interpolated linearly across the F oversampled sub-samples of each base sample, and `1/g` is computed per sub-sample. There is no base-rate staircase.
- **Type changes** are a true **20 ms crossfade in the oversampled domain.**
  - Both curves run on the same upsampled input, and the weight advances per oversampled sample: `w = (fadePos·F + k + 1) / (fadeLength·F)`, with `fadeLength = round(0.02·fs)`.
  - The DC blocker and head bump run continuously; their weights follow the fade.
  - Fading *into* Tape resets its emphasis states first, because the other curves are stateless.
  - Selecting the previous type mid-fade reverses the fade from its current position. A third type waits until the running fade ends.
- **Segment splitting.** `process()` splits blocks only at fade ends and at `maxBlockSize`. Everything else is per-sample state, so the output is identical for any host block size, up to the timing of sub-1e-15 state flushes.
- **Timing skew during ramps.** While drive ramps or a type fades, the base-rate weights (depth, head bump, DC blocker) lead the oversampled curve weights by about L/2 base samples: the upsampler and the downsampler each add about L/2. That is 16 samples (0.33 ms) at 48 kHz with 2× High. This is inaudible within the 20 ms ramps.
- **Module on/off** uses the `ModuleSlot` 20 ms crossfade against the delayed dry path. On re-activation the module is reset and pre-rolled for latency + 64 samples before it is heard.

### 6.7 Latency & CPU

- **Latency** equals the oversampler's round trip (§0.6): 0 / 32 / 36 samples (1× / 2× High / 4× High) and 16 / 19 samples (2× Low / 4× Low).
  - It is structural: `setOversampling()` takes effect at the next `prepare()`.
  - The dry path is delayed by the same L.
  - Test *Saturator: latencySamples() is exact - a low-level impulse appears L samples later* checks this.
- **CPU** (indicative, drive 12 dB, re-measured with delta oversampling; run-to-run spread about ±10 %). The cost of `std::tanh` dominates Tape and Tube; the delta path adds only a copy and a subtraction per oversampled sample. During a type crossfade the curve cost doubles for 20 ms. The wet path runs even at 0 dB drive (warm states, click-free drive-up); `ModuleSlot` bypass is the way to save CPU.

| Configuration | Tape | Tube | Digital |
|---|---|---|---|
| 1× | 64 ns (0.31 %) | — | — |
| 2× Low (Balanced / Low Latency) | 143 ns (0.69 %) | 117 ns (0.56 %) | 58 ns (0.28 %) |
| 2× High (Quality) | 170 ns (0.82 %) | 138 ns (0.66 %) | 83 ns (0.40 %) |
| 4× High | 297 ns (1.43 %) | — | — |

### 6.8 Gaming vs Music usage

- **Music.**
  - **Warmth** engages Saturation (`sat.on`) once the macro exceeds about 1 %: the toggle contribution is `smoothstep(0, 0.02, v)` and a toggle reads as on at ≥ 0.5. It also adds drive +9 dB (governed).
  - **Boost Intensity** adds drive +4 dB over 60–100 % (governed).
  - The SafetyGovernor can take this drive back when the maximizer limits too hard or the measured THD+N of the saturator and the clipper together exceeds −30 dB (§14.5).
- **Gaming.**
  - Nothing in the gaming macro table engages or drives saturation, and `sat.on` defaults to off. Saturation therefore stays off in Gaming mode unless a preset or the user turns it on.
  - This is deliberate: added harmonics and peak rounding bring no benefit to positional cues.
  - If it is enabled, the Low Latency profile runs it at 2× Low (16 samples).

### 6.9 Tests that prove it (`tests/test_saturator.cpp`)

- **Curves and level:**
  - *Saturator: shape() has f(0) = 0, unit slope at 0, is bounded and monotonic*
  - *Saturator: small-signal gain is unity (-40 dBFS, 1 kHz, drive 12 dB)* (±0.1 dB for every type and mode; flat within 0.02 dB across the band at −60 dBFS)
  - *Saturator: drive 0 dB is transparent and mix 0 is the exactly delayed dry signal*
  - *Saturator: output gain scales the wet path and the dry/wet mix is latency-aligned*
  - *Transparency: the saturator keeps a quiet 19.5 kHz tone at unity gain with drive engaged* (`tests/test_transparency.cpp`: Tape, Tube and Digital at 44.1 and 48 kHz, 2× Low and High, drive 12 dB, −40 dBFS, within 0.1 dB)
- **Harmonics, DC and aliasing:**
  - *Saturator: THD rises with drive for every type*
  - *Saturator: Tube has a strong 2nd harmonic, Tape and Digital are odd*
  - *Saturator: Tube output is DC-free after settling*
  - *Saturator: 4x HQ Digital keeps 15 kHz aliasing below -60 dB (1x does not)*
  - *Saturator: latencySamples() is exact - a low-level impulse appears L samples later*
  - *Saturator: tape head bump follows drive and HF saturates earlier (emphasis)*
- **Click-freeness:**
  - *Saturator: drive / output / mix jumps are click-free*
  - *Saturator: type changes are crossfaded without clicks*
  - *Saturator: reversed and queued type changes converge to a fresh instance*
- **RT safety and robustness:**
  - *Saturator: process, reset and setParams do not allocate*
  - *Saturator: robust to silence, DC, full-scale noise, impulses and extreme parameters at every rate*
  - *Saturator: channels are independent; fewer channels than prepared is fine*
  - *Saturator: output is independent of the host block size*
- **Review regressions:**
  - *Saturator (review): no subnormal crawl in the tape emphasis after the input stops (FTZ off)*
  - *Saturator (review): recovers from a NaN / Inf input burst*
  - *Saturator (review): mix 0 stays the exact delayed dry signal during fades and ramps*
  - *Saturator (review): maxBlockSize 1 gives the same output as maxBlockSize 4096*
  - *Saturator (review): toggling the type every sample stays click-free*

### 6.10 Known limitations

- **Aliasing at the chain's 2× setting.** Heavy saturation of loud HF content aliases (table in §6.4). ADAA is roadmap. The 4× modes are available through the API but are not used by any latency profile.
- **Loud material gets quieter as drive rises.** There is no automatic make-up (§6.3).
- **The head bump is a simplified model** (constant-Q peak, no dip).
- **Tube curve rounding.** The Tube curve can be non-monotonic by one float ulp (about 6e-8) next to its positive asymptote. This comes from rounding in `T/(1 + tT)` and is inaudible.

---

## 7. Stereo widener / spatializer

Sources: [`core/include/flub/dsp/StereoSpatializer.h`](../core/include/flub/dsp/StereoSpatializer.h), [`core/src/dsp/StereoSpatializer.cpp`](../core/src/dsp/StereoSpatializer.cpp).

### 7.1 Purpose

"Stereo & Space" widens, focuses and decorrelates the stereo image. It also offers bs2b-style headphone crossfeed. Every stage acts on the **side** signal only, so the mono fold-down is invariant for every setting (§7.3.6). That guarantee protects mono playback, laptop speakers and Bluetooth hands-free links from comb filtering.

- Zero latency, all IIR.
- Stereo only: a block with `numChannels != 2` passes through untouched, and no state advances.
- Its control rate is `fs / 32` (`kControlInterval = 32`). The only control-rate work is the mono-safety loop and state hygiene; every smoother runs per sample.

### 7.2 Signal flow

```
 L ─┐   M = (L + R) / 2 ─────────────────────────────────────────────────────┬─► L' = M + S4
 R ─┘   S = (L − R) / 2                                                       └─► R' = M − S4
        │
        S  ─► HS_w: 2nd-order high shelf (Q 1/√2) at spatial.lowCut ─► × min(w, 1) ─► S1   width
        S1 ─► S1 + guard · (bell 3 kHz, Q 0.5, +6 dB · focus (S1) − S1) ────────► S2   positional focus
        M, S1 ─► band-pass 3 kHz, Q 0.5 ─► envelopes e_M, e_S ─► guard ∈ [0, 1]   (polarity guard, §7.3.2)
        M  ─► HP 300 Hz (Butterworth, 12 dB/oct) ─► z^−5 ms ─► D (nested all-pass) ─► × 0.5·space ─(+)─► S3
        S3 ─► S3 − 0.6 · crossfeed · LP1_700 Hz(S3) ───────────────────────────────► S4   crossfeed

 L', R' ─► <L'R'>, <L'²>, <R'²> (one-pole 300 ms, double) ─► ρ
        ─► every 32 samples: safety s ∈ [0, 1] ─► w_eff = w > 1 ? 1 + (w − 1)(1 − s) : w ─► 20 ms smoother ─► w
```

### 7.3 Algorithm & maths as implemented

#### 7.3.1 Width: a complementary shelf, not an LR4

The header contract is `S' = S_low · min(w, 1) + S_high · w`, split at `widthLowCutHz`. It is realised with a **complementary** split (`S_low + S_high = S` exactly):
- for `w ≤ 1` the width is the plain gain `w · S`;
- for `w > 1` it is `S + (w − 1) · S_high`, i.e. a 2nd-order minimum-phase high shelf on S.

The shelf is the Cytomic high shelf of §0.2, written in terms of the width so that the end points are exact:

```
wHi = max(w, 1),   A = sqrt(wHi)                (shelf gain A² = wHi)
g   = tan(π · lowCut / fs) · sqrt(A),   k = √2  (Q = 1/√2, no overshoot)
m0  = wHi,   m1 = k (1 − A) A,   m2 = 1 − wHi   → m0 + m2 = 1 exactly (LF gain 1)
S1  = min(w, 1) · SVF(S)                         w = 1 gives (1, 0, 0): bit-exact identity
```

The magnitude is 0 dB below the cut, `20 log10 w` above it and half of that (in dB) at the cut. Analytic values at `w = 2`, `lowCut = 180 Hz`, 48 kHz:

| Frequency re cut | 0.22× | 0.5× | 1× | 2× | 4× |
|---|---|---|---|---|---|
| S gain, complementary shelf (as built) | +0.02 dB | +0.38 dB | +3.01 dB | +5.64 dB | +6.00 dB |
| S phase re M | +6.5° | +16.6° | +27.7° | +16.6° | +7.5° |
| in-phase LR4 band sum (reference) | — | +0.5 dB | +3.5 dB | +5.8 dB | — |

The phase of S never strays more than 27.7° from M (16.3° at `w = 1.5`). A literal LR4 pair sums to a 2nd-order all-pass that reaches −180° at the crossover (§0.4). Applied to S alone, with M untouched, it would invert the side signal around the low cut: a left-panned 180 Hz source would image to the right. Width 1 could then never be a bit-exact pass-through either. Test *StereoSpatializer (review): widening never mirrors a panned source around the low cut* pins this choice.

The shelf is re-derived per sample only while `w` or the low cut glide: two square roots and one division, plus an `exp` and a `tan` while the low cut moves.

#### 7.3.2 Positional focus

A Cytomic bell (as `SvfCoeffs::make(Bell)`) on S1 at a fixed 3 kHz, Q 0.5, gain `6 dB · positionalFocus`:

```
A = 10^(gainDb / 40),  k = 1 / (Q A),  m = (1, k (A² − 1), 0)       focus 0 → m1 = 0 exactly
```

At focus 1 and 48 kHz (analytic): +0.25 dB at 300 Hz, +2.15 dB at 1 kHz, **+6.00 dB at 3 kHz**, +3.63 dB at 6 kHz, +1.46 dB at 10 kHz and +0.44 dB at 15 kHz. The gain is ≥ +3 dB from 1.26 kHz to 6.84 kHz. For broadband transients (footsteps, reloads), interaural level differences in this region are the main lateral localisation cue. Emphasising S there sharpens the perceived direction without touching the centre.

**Polarity guard.** Raising S by a gain G against an untouched M turns the far ear `M − G·S` negative once `G·S > M`. For a source panned hard to one side (R = 0, so M = S) any lift would do so and put an anti-phase copy in the silent ear, taking the ILD from infinite to about 10 dB. The bell's *added* signal is therefore applied only in the share that keeps the quieter ear's polarity:

```
e_M, e_S = peak-hold envelopes of BP(M), BP(S1)       BP: band-pass at the bell's own 3 kHz, Q 0.5; release 30 ms
G        = A²                                          linear bell gain at 3 kHz
guard    = clamp((e_M − e_S) / (e_S · (G − 1)), 0, 1)  instant attack, 50 ms release
S2       = S1 + guard · (Bell(S1) − S1)                focus 0: Bell(S1) == S1, bit-exact
```

- At the full bell (+6 dB) the guard lets everything through while `e_M ≥ 2·e_S`, i.e. for sources panned less than about 9.5 dB (R ≥ L/3). Harder-panned sources get only the share that brings the far ear towards silence, never past it.
- A hard-panned source (`e_M = e_S`) gets no lift, so its far ear stays exactly silent. A partially panned one still gains ILD: R = L/2 goes from 6 dB to 14 dB at 3 kHz (focus 1, width 1).
- A pure side signal (`e_M = 0`) is not lifted, and the guard reads S1, i.e. after the width: where width has already raised S above M, focus adds nothing.
- M is still never written, so the mono sum stays exact (§7.3.6). The guard is one gain for the whole band, derived from the band mix, not per source (§7.9).

#### 7.3.3 Space

```
x  = z^−P · HP300(M)               HP300: 2nd-order Butterworth SVF high-pass, P = 5 ms pre-delay
D  : three nested Schroeder all-passes, g = 0.5; the outer (7.3 ms) section carries the
     4.7 ms section in its delay path, which carries the 3.1 ms one. Per section:
         v = in + g · w,   y = w − g · v,   w = inner(v delayed)
S3 = S2 + 0.5 · space · D(x)
```

- Delays are whole samples (`msToSamples`, rounded) in power-of-two lines that share one wrapped write index. All reads precede the writes, and every delay is ≥ 1 sample.

  | fs | pre-delay | outer | middle | inner |
  |---|---|---|---|---|
  | 44.1 kHz | 221 | 322 | 207 | 137 |
  | 48 kHz | 240 | 350 | 226 | 149 |
  | 96 kHz | 480 | 701 | 451 | 298 |
  | 192 kHz | 960 | 1402 | 902 | 595 |

- D is a true lossless all-pass, so the ambience has exactly the spectrum of HP300(M). On mono sines at space 1, S/M (analytic) is −29.0 dB at 80 Hz, −9.0 dB at 300 Hz, −6.06 dB at 1 kHz and −6.02 dB from 3 kHz. On white noise the test measures −6.1 dB.
- **Why the pre-delay.** A Schroeder section passes `−g` of its input with no delay. Without P that instantaneous tap would put `−0.25 · space · HP(M)` straight into S and pan the centre sideways (4.4 dB interaural level difference at space 1). Delayed by 5 ms it becomes a lateral early reflection (a Lauridsen-type complementary comb) that reads as space, not as an image shift, and the ambience is uncorrelated with M at lag 0.
- **Decay** (measured, impulse on M, space 1, 48 kHz, 10 ms windows): −20 dB after 80 ms, −40 dB after 190 ms, −60 dB after 330 ms; the tail falls about 125–130 dB/s. The flush in §7.3.7 makes it exactly zero after about 2.1 s.
- The network always runs, even at space 0, so raising space starts from a live tail.

#### 7.3.4 Crossfeed

A first-order TPT low-pass at 700 Hz, subtracted from S:

```
G  = tan(π·700/fs) / (1 + tan(π·700/fs))
v  = (S3 − z) · G,   lp = v + z,   z ← lp + v
S4 = S3 − 0.6 · crossfeed · lp
```

This is a monotonic low shelf on S only, with no resonance. At crossfeed 1 (analytic, 48 kHz): −7.96 dB at DC, −7.53 dB at 100 Hz, −5.37 dB at 300 Hz, −2.37 dB at 700 Hz, −1.40 dB at 1 kHz, −0.19 dB at 3 kHz and −0.02 dB at 8 kHz. It reduces low-frequency separation on headphones (bs2b-like comfort) without colouring M.

#### 7.3.5 Output

`L' = M + S4`, `R' = M − S4`. When every stage is neutral, S4 equals S bit-exactly and the code leaves L and R untouched, because `M + S` does not round back to L in general. The neutral path is therefore bit-exact (test *width 1 with everything else neutral is a bit-exact pass-through*).

#### 7.3.6 Mono-sum invariance: proof

Claim: for every parameter setting, every automation trajectory and every safety state, `L' + R' = L + R`, up to float rounding.

1. In exact arithmetic, `L' + R' = (M + S4) + (M − S4) = 2M = L + R`. This holds whatever S4 is, because S4 cancels.
2. Nothing in the module writes to M:
   - width, focus, crossfeed and the safety only scale or filter S (the focus's polarity guard reads M but writes only S);
   - the ambience is *derived* from M but *added* to S.

   So the identity holds sample by sample for any time-varying, recursive or nonlinear processing of S, including parameter glides and the safety loop.
3. Float rounding. `mid = fl(0.5 · fl(L + R))`, `L' = fl(mid + s)` and `R' = fl(mid − s)`, with unit roundoff `u = 2^−24`:
   ```
   |(L' + R') − (L + R)| ≤ u · (|L'| + |R'| + |L + R|)          (a few ulp of the signal)
   ```
   The neutral path (S4 == S) is exact.
4. Measured: 40 runs of full-scale white noise, every parameter re-randomised every 256 samples (48 kHz). The worst `|L' + R' − (L + R)|` was **1.19e−7**. The test bound is 1e−5 (*StereoSpatializer: L'+R' == L+R for random stereo noise under random settings*; also checked under per-sample parameter thrash).

Mono invariance is not the same as "nothing changes in mono". The space ambience and widened S *cancel* in the mono sum; they do not leak into it.

#### 7.3.7 Auto mono safety

```
correlation (output, double): <L'R'>, <L'²>, <R'²> one-pole, τ = 300 ms
ρ = <L'R'> / sqrt(<L'²><R'²>),  clamped to [−1, 1];  undefined (reads 1) if <L'²><R'²> ≤ 1e−20 (≈ −100 dBFS)

every 32 samples of stream time:
  safety off, or user width ≤ 1 :  s −= 32 / (0.3 s · fs)                  released in 300 ms
  ρ undefined (near silence)    :  hold
  else, e = minCorrelation − ρ, h = min(0.05, (1 − minCorrelation) / 2):
      e > 0   :  s += 32 / (0.3 s · fs) · min(1, e / 0.1)                  full pull in 300 ms
      e < −h  :  s −= 32 / (3 s · fs)                                       slow 3 s release
      else    :  hold                                                       hysteresis band
  s = clamp(s, 0, 1)
w_eff = w > 1 ? 1 + (w − 1)(1 − s) : w            only widening is pulled back
```

- `h` shrinks near `minCorrelation = 1` so that release stays reachable (test *mono safety releases with minCorrelation close to 1*).
- Holding during near-silence means game pauses between events do not pump the width back up.
- Per-tick steps at 48 kHz: 2.22e−3 (pull at full error) and 2.22e−4 (release). They scale with fs, so the timing is rate-independent (test *mono safety timing does not depend on the sample rate*).
- Measured (48 kHz, `w = 2`, `minCorrelation = 0`, antiphase noise): `w_eff ≤ 1.05` after 0.31 s. After switching to mono content, `w_eff ≥ 1.95` after 3.10 s.
- `getCorrelation()` and `getEffectiveWidth()` are published once per stereo block (relaxed atomics). The chain forwards the effective width to `MeterBus::effectiveWidth`. The chain's correlation meter is the separate `LevelMeter` (section 13).

**State hygiene** also runs on the 32-sample tick, in stream time:
- SVF states (including the polarity guard's two band-passes), the guard envelopes and the crossfeed state below 1e−15 are flushed;
- all-pass line writes are flushed per write;
- the correlation accumulators are zeroed when `<L'²> + <R'²> < 1e−30`;
- any non-finite filter state, ambience output or correlation sum clears all state.

A NaN or ±inf input is therefore contained within 32–64 samples, and flushing on the tick instead of per host block keeps the output bit-identical for any block size.

### 7.4 Parameters

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Stereo & Space | `spatial.on` | off/on | on | toggle | module bypass (`ModuleSlot`, 20 ms) |
| Width | `spatial.width` | 0 … 2 | 1 | % (stored 0–2 = 0–200 %) | S gain; above 1 only above the low cut (§7.3.1) |
| Width Low Cut | `spatial.lowCut` | 60 … 500 | 180 | Hz | shelf corner (half-gain point) |
| Positional Focus | `spatial.focus` | 0 … 1 | 0 | % | 3 kHz bell on S, 0 … +6 dB, lift bounded by the polarity guard (§7.3.2) |
| Space | `spatial.space` | 0 … 1 | 0 | % | ambience level, `0.5 · space · D(…)` into S |
| Headphone Crossfeed | `spatial.crossfeed` | 0 … 1 | 0 | % | `0.6 · crossfeed` of LP1_700(S) subtracted |
| Mono Safety | `spatial.monoSafety` | off/on | on | toggle | enables the correlation loop (§7.3.7) |
| Min Correlation | `spatial.minCorrelation` | −1 … 1 | 0 | — | target output correlation for the safety |

Module-level sanitising: NaN keeps the previous value; everything else, ±inf included, clamps. An unchanged parameter set returns early.

### 7.5 Smoothing & click-freeness

| Change | Mechanism | Time |
|---|---|---|
| width (user × safety) | per-sample one-pole on `w_eff`; shelf re-derived per sample while it moves | 20 ms (also removes the 32-sample steps of s) |
| low cut | per-sample one-pole on ln(Hz) | 50 ms |
| focus (dB), space gain, crossfeed gain | per-sample one-poles; bell a-coefficients re-derived while focus moves | 20 ms |
| focus polarity guard | band envelopes: peak hold, one-pole release; guard: instant attack, one-pole release (a signal-driven gain, not a parameter) | 30 ms / 50 ms |
| module on/off | `ModuleSlot` crossfade against the (zero-latency) dry path | 20 ms |
| `reset()` | every smoother jumps to its target (nothing to click against) | — |

There are no control-rate coefficient steps. The output is bit-identical for block sizes 1/13/1024/4096, including decaying tails (test *bit-identical for any block size, including tails and max-size blocks*).

### 7.6 Latency & CPU

- **Latency: 0** (test *zero latency - an impulse comes out at its own sample*).
- Per stereo sample: five SVF ticks (shelf, bell, the polarity guard's two band-passes, HP), one first-order low-pass, at most one division (the guard), four delay-line reads and writes, three double multiply-adds. Coefficient maths run only while a parameter glides.
- CPU (indicative, conditions under *Reading conventions*): **26–27 ns** per stereo sample neutral (0.12–0.13 % of a core; the ambience network, the guard's detectors and the correlation run even at neutral settings), **31–32 ns** with width 1.6 and focus/space/crossfeed 0.5 (0.15 %). The polarity guard added about 7–8 ns (19–20 / 23–24 ns before it).
- Memory: four delay lines, the largest 2048 floats at 192 kHz, allocated in `prepare()`.

### 7.7 Gaming vs Music usage

| | Music | Gaming |
|---|---|---|
| Macros | **Width** macro: engages `spatial.on`, width +0.6 (0–100 %), space +0.35 (40–100 %). **Boost Intensity**: width +0.2 (0–50 %). | **Positional** macro: engages `spatial.on`, focus +0.9 (0–100 %), width +0.25 (30–100 %); it raises the ILD of partially panned sources and leaves hard-panned ones hard-panned (the focus's polarity guard, §7.3.2; the mono safety pulls the width back, §7.9). **Boost Intensity**: focus +0.3 (0–60 %). |
| Crossfeed | as set by the user / preset | **forced to 0** by `ProcessingChain` (it blurs interaural differences, the main lateral cue) |
| Binaural lock (both modes) | When a 5.1/7.1 strip was rendered by the virtualiser (`virt.on`), the chain forces width 1, space 0 and crossfeed 0: binaural output already carries exact interaural cues. Focus stays available. | same |

The chain writes these overrides into the effective values, so `effectiveValue()` and the GUI's effective-value rings show what the spatializer applies (§14.1). All contributions are ungoverned (they add little loudness). Macro contributions are clamped to the parameter range: at 100 % Music Width plus Boost, width is `1 + 0.6 + 0.2 = 1.8`.

### 7.8 Tests that prove it (`tests/test_spatializer.cpp`)

- **Mono guarantee and exactness:**
  - *StereoSpatializer: L'+R' == L+R for random stereo noise under random settings* (≤ 1e−5)
  - *StereoSpatializer: width 1 with everything else neutral is a bit-exact pass-through*
  - *StereoSpatializer: width 0 folds to mono, L' == R' == (L + R) / 2*
- **Responses:**
  - *StereoSpatializer: width 2 lifts S by 6 dB above the low cut and not below; M untouched* (+6.02 ± 0.1 dB at 2/10 kHz, ≤ 0.1 dB at 40 Hz, ≤ 0.6 dB at half the cut; matches the analytic shelf within 0.05 dB)
  - *StereoSpatializer: positional focus lifts S around 3 kHz only; M untouched* (measured on a panned source, M = 4 S, so the guard allows the full bell: +6.0 ± 0.05 dB at 3 kHz, the analytic bell within 0.05 dB; a pure side signal gets ≤ 0.01 dB)
  - *StereoSpatializer: positional focus never flips the far ear - hard-panned sources stay hard-panned* (focus 1, hard-left 3 kHz sine and white noise: right ear exactly 0, left ear bit-identical; R = L/2 at 3 kHz: ILD ≥ 12 dB, analytic 14 dB, mono-sum error ≤ 1e−6)
  - *StereoSpatializer: space adds decorrelated S to a mono input; mono sum stays exact* (S/M −6.1 ± 0.5 dB on noise; −6.02 ± 0.05 dB at 3 and 9 kHz; ≤ −25 dB at 80 Hz)
  - *StereoSpatializer: crossfeed reduces low-frequency S only; M untouched*
- **Safety and metering:**
  - *StereoSpatializer: auto mono safety pulls the width back for antiphase-heavy content*
  - *StereoSpatializer: published correlation tracks the output*
- **Channels, RT safety, robustness, timing:**
  - *StereoSpatializer: 1-channel and 6-channel blocks pass through untouched*
  - *StereoSpatializer: no allocation in reset / setParams / process*
  - *StereoSpatializer: robustness - silence, DC, full-scale noise, impulses, extreme params, all rates*
  - *StereoSpatializer: output is independent of the host block size*
  - *StereoSpatializer: zero latency - an impulse comes out at its own sample*
  - *StereoSpatializer: parameter jumps are click-free*
- **Review regressions:**
  - *StereoSpatializer (review): bit-identical for any block size, including tails and max-size blocks*
  - *StereoSpatializer (review): mono safety releases with minCorrelation close to 1*
  - *StereoSpatializer (review): mono safety timing does not depend on the sample rate*
  - *StereoSpatializer (review): widening never mirrors a panned source around the low cut*
  - *StereoSpatializer (review): NaN / inf inputs are contained within one control interval*
  - *StereoSpatializer (review): per-sample parameter thrash stays finite, bounded and mono-exact*
  - *StereoSpatializer (review): re-prepare at another rate, empty blocks, silence start*

### 7.9 Known limitations

- **Shelf, not brick-wall.** Below the cut the width transition falls at about 12 dB/oct, similar in practice to an in-phase LR4 sum but not a 24 dB/oct split.
- **Correlation reads 1 when one output channel is silent.** "Silent" here means `<L'²><R'²> ≤ 1e−20`, a geometric-mean level of about −100 dBFS; an example is a hard-panned source at width ≤ 1, at any focus. This is the same convention as `LevelMeter`; many meters read 0 there. The safety holds its state while ρ is undefined.
- **The safety scales only the width.** Space and focus are not pulled back, so a low `minCorrelation` target can be missed while space is on (the width then saturates at 1, which is harmless).
- **Hard-panned sources widened above 1 always drive ρ towards −1**, so any `minCorrelation ≥ 0` pulls such material fully back to width 1. This follows from the contract.
- **Width above 1 lowers the ILD of a hard-panned source; positional focus does not.** Width scales the side signal above the low cut. For a source on one channel only (R = 0, so M = S), a side gain g > 1 gives `L' = (1 + g)·L/2` and `R' = (1 − g)·L/2`: an anti-phase copy in the far ear, and the interaural level difference falls from infinite to `20 log10((1 + g)/(g − 1))` (19.1 dB at the *Positional* macro's width 1.25). The mono safety pulls such width back to 1 (previous point). Focus no longer lowers it: its polarity guard (§7.3.2) gives a hard-panned source no lift. Through the chain, the Gaming *Positional* macro at 100 % (with or without Boost Intensity at 100 %) leaves the far ear of a hard-left tone at 1, 2, 3, 6 or 10 kHz, or of hard-left white noise, at numerical silence; before the guard the ILD fell to 10.4 dB at 3 kHz (9.6 dB with Boost). Partially panned sources still gain ILD: an R = L/2 source goes from 6 dB to 18.0 dB at 3 kHz (20.7 dB with Boost Intensity also at 100 %). The mono sum is unchanged in every case (§7.3.6). *Gaming Positional (M2): …* in `tests/test_modes.cpp` asserts that a hard-left 3 kHz tone keeps at least 60 dB of ILD and its near-ear level within 0.5 dB.
- **The polarity guard works on the band mix, not per source.** It compares band envelopes of M and S around 3 kHz, so a hard-panned sound under a louder centred one (`e_M > e_S`) is still lifted, and its anti-phase copy lands in the far ear under the centred sound. Measured through the chain at *Positional* 100 %: a hard-left 3.5 kHz tone 12 dB below a centred 2.5 kHz tone reaches the right ear 8.1 dB below its left-ear level. Conversely, material whose band S is at least as strong as its M (very wide or anti-phase content, a pure side signal) gets no focus lift at all. Raising only the near ear of every source would need to know where each one is panned, which an M/S processor does not.
- **The space network is fixed:** no size, decay or modulation controls. Like any decorrelator, it gives frequency-dependent level differences between the ears on steady tones.
- **The loop can overshoot once** on a sudden change, because of the 300 ms measurement lag. The implementer observed a dip to about 1.19 before settling at 1.33 on content with input correlation 0.3. It does not hunt.

---

## 8. Headphone virtualizer

Sources: [`core/include/flub/dsp/HeadphoneVirtualizer.h`](../core/include/flub/dsp/HeadphoneVirtualizer.h), [`core/src/dsp/HeadphoneVirtualizer.cpp`](../core/src/dsp/HeadphoneVirtualizer.cpp). Chain integration: `ProcessingChain::process()` step 2.

### 8.1 Purpose

Games render true positional audio when the endpoint reports 7.1, so the "Flubsound Game" endpoint advertises 7.1. This module folds 5.1/7.1 (or two virtual stereo speakers) down to **binaural** stereo for headphones: the "virtual 7.1" idea. It has two renderers:

- **A. Parametric** (built in, no data licence): a Brown & Duda (1998) spherical head. It provides Woodworth ITD, a first-order head shadow, a rear pinna cue and early reflections.
- **B. Measured HRIRs**: direct-form time-domain convolution of a per-speaker left/right impulse-response set.

Both renderers share the LFE path, the room reflections and a −3 dB headroom trim. The module reports **zero latency**: the ITD delays are part of the acoustic model (a centre source reaches both ears after a/c), not added latency.

### 8.2 Signal flow

```
 channel map (WAVEFORMATEXTENSIBLE):  5.1 = FL FR FC LFE SL SR     7.1 = FL FR FC LFE BL BR SL SR
 azimuth (deg, + = right):            FL/FR ∓front (30)  FC 0  SL/SR ∓side (100)  BL/BR ∓rear (145)

 speaker x (renderer A) ─► rear-cue shelf ─► ITD line ─┬─► Lagrange(D_L) ─► shadow_L ─► ear L ─┐
                                                       └─► Lagrange(D_R) ─► shadow_R ─► ear R ─┤
 speaker x (renderer B) ─► 2L history ─► dot(h_L) ─► ear L,  dot(h_R) ─► ear R ────────────────┤
 LFE ─► LP 120 Hz (Butterworth, 24 dB/oct) ─► × lfeGain ─► both ears ──────────────────────────┤
 Σ non-LFE speaker inputs ─► HP 200 Hz ─► LP 5 kHz ─► 6 taps 4–19 ms, alternating ears ─► × room ┤
                                                                                                ▼
                               (ear L, ear R) × 0.70795 (−3 dB trim) × swap fade ─► ch 0 / ch 1; ch ≥ 2 cleared
```

In the chain the module runs only on multichannel strips (`inputChannels > 2`) with `virt.on`, after the input gain and AutoLevel and before any slot. The chain picks the layout from the strip width: 8 channels are read as 7.1, 6–7 as 5.1 and 3–5 as the stereo layout (FL/FR only; the rest is ignored and cleared). With `virt.on` off the chain applies an ITU-R BS.775 downmix instead: `L = 0.7071 · (FL + 0.7071·FC + 0.7071·(BL + SL))`, likewise R, with the LFE dropped.

Toggling `virt.on` crossfades the two folds (`ProcessingChain::virtMix`, 20 ms linear). While the fade runs, both are computed: the downmix works on a copy of the input in `foldScratch`, and the output is `y = d + w · (v − d)`. Outside a fade only one fold runs. When the virtualiser is switched on from fully off it is `reset()` first, so it starts from silence rather than from the history it had when it was switched off.

### 8.3 Algorithm & maths as implemented

#### 8.3.1 Geometry and Woodworth ITD

Constants: `c = 343 m/s`, head radius `a = virt.headRadius` (default 87.5 mm, so a/c = 255.1 µs = 12.24 samples at 48 kHz). For a speaker at azimuth `az` and an ear at −90° (left) or +90° (right):

```
θ = |remainder(az − earAz, 360°)| ∈ [0°, 180°]     angle between source and ear axis
D = (a/c)(1 − cos θ) · fs              θ < 90°      (path to the ear on the lit side)
  = (a/c)(1 + θ − π/2) · fs            θ ≥ 90°      (straight line to the tangent + arc round the head)
```

For a source at lateral angle φ, the near ear has θ = 90° − φ and the far ear θ = 90° + φ:

```
ITD = D_far − D_near = (a/c)·[(1 + φ) − (1 − sin φ)] = (a/c)(φ + sin φ)          Woodworth
```

A rear source at 145° has the same lateral angle (35°) as one at 35°: a sphere is front/back symmetric, so the rear cue below is what separates them.

Per-ear delays and ITDs at the defaults (a = 87.5 mm, 48 kHz; analytic):

| Speaker | az | θ_L / θ_R | D_L / D_R (samples) | ITD |
|---|---|---|---|---|
| FL | −30° | 60° / 120° | 6.12 / 18.66 | 261 µs |
| FC | 0° | 90° / 90° | 12.24 / 12.24 | 0 |
| SL | −100° | 10° / 170° | 0.19 / 29.34 | 607 µs |
| BL | −145° | 55° / 125° | 5.22 / 19.72 | 302 µs |

The maximum per-ear delay is `(a/c)(1 + π/2)`: 655.8 µs at the default radius and 787.0 µs = 151.1 samples at 192 kHz for a = 105 mm. The ITD lines are sized for that (+ 8, rounded up to a power of two): 256 samples up to 192 kHz, 512 at 384 kHz and 1024 at 768 kHz. The line can be shorter than a host block, so each speaker runs one fused per-sample loop that writes a sample and reads its four Lagrange taps before the line wraps.

**Fractional delay** is a 3rd-order (4-tap) Lagrange interpolator:

```
base = max(0, floor(D) − 1),  d = D − base ∈ [1, 2)   (d ∈ [0, 1) when D < 1)
h0 = −(d−1)(d−2)(d−3)/6,  h1 = d(d−2)(d−3)/2,  h2 = −d(d−1)(d−3)/2,  h3 = d(d−1)(d−2)/6
y  = Σ_k h_k · x[n − base − k]
```

The taps are continuous in D, and an exact integer delay selects a single tap. At the worst position (d = 1.5) its magnitude is −0.54 dB at 10 kHz, **−3.25 dB at 16 kHz** and −8.4 dB at 20 kHz (48 kHz, analytic). So different ears and angles get slightly different top octaves.

#### 8.3.2 Brown–Duda head shadow

A first-order shelving filter per ear:

```
H(s) = (1 + α s / (2 ω0)) / (1 + s / (2 ω0)),   ω0 = c / a
α(θ) = 1.05 + 0.95 · cos(θ · 180°/150°)
DC gain 1;  HF gain α;  pole at 2ω0 → c/(π a) = 1247.8 Hz for a = 87.5 mm
```

`α` is 2.0 (+6.02 dB) facing the ear (θ = 0°) and 0.1 (−20 dB) at θ = 150°. At θ = 180° it recovers partly to 0.28 (−11.0 dB), the "bright spot" behind a sphere. The section is bilinear-transformed **without prewarping** (`BiquadCoeffs::fromAnalogFirstOrder`, K = 2 fs), so the Nyquist gain is exactly α, and it runs as a double-precision TDF-II section.

Per-ear shadow magnitude at the defaults (analytic, excludes trim and rear shelf):

| Speaker | ear L: α / 1 kHz / 4 kHz / 10 kHz | ear R: α / 1 kHz / 4 kHz / 10 kHz | ILD at 4 kHz |
|---|---|---|---|
| FL (−30°) | 1.34 / +1.2 / +2.4 / +2.5 dB | 0.28 / −1.9 / −8.0 / −10.5 dB | 10.4 dB |
| FC (0°) | 0.76 / −0.8 / −2.2 / −2.4 dB | 0.76 / −0.8 / −2.2 / −2.4 dB | 0 |
| SL (−100°) | 1.98 / +3.3 / +5.6 / +5.9 dB | 0.18 / −2.1 / −9.4 / −13.6 dB | 15.0 dB |
| BL (−145°) | 1.44 / +1.5 / +3.0 / +3.1 dB | 0.23 / −2.0 / −8.8 / −12.1 dB | 11.8 dB |

#### 8.3.3 Rear cue

In renderer A every speaker passes an SVF high shelf at 4 kHz, Q 0.7071. Its gain is **−4 dB for |azimuth| > 90°** and 0 dB otherwise: −2 dB at 4 kHz (the half-gain point, §0.2) and −4 dB above. This models pinna shadowing of sources behind the head and resolves the sphere's front/back symmetry.
- At the defaults it applies to BL/BR (145°) and also to SL/SR, because the default side angle, 100°, is past 90°.
- A 0 dB shelf is an exact identity, but it still runs on every speaker so its state stays warm.
- When a side angle crosses 90°, the shelf gain glides (10 ms) rather than stepping.

#### 8.3.4 LFE

A 4th-order Butterworth low-pass at 120 Hz: two SVF sections, Q 0.5412 and 1.3066. It is scaled by `dbToGain(virt.lfe)` and sent identically to both ears. Including the trim, the implementer measured −3.0 dB at 50 Hz, −20.8 dB at 200 Hz and −76.7 dB at 1 kHz; the analytic Butterworth values agree. The stereo layout has no LFE.

#### 8.3.5 Early reflections

```
bus  = Σ non-LFE speaker inputs  → SVF HP 200 Hz (Q 0.7071) → SVF LP 5 kHz (Q 0.7071) → power-of-two line
ear L += room · (0.5 · bus[n − 4.0 ms] + 0.4 · bus[n − 8.9 ms] + 0.3 · bus[n − 15.4 ms])
ear R += room · (0.5 · bus[n − 5.3 ms] + 0.4 · bus[n − 11.7 ms] + 0.3 · bus[n − 19.0 ms])
```

- Reflected energy per ear is `(0.5² + 0.4² + 0.3²) · room² = 0.5 · room²`: −3 dB re a unit direct path at room 1 and −19.5 dB at the default 0.15, before band-limiting.
- The delays are mutually non-harmonic, to avoid a periodic comb, and integer: 192/254/427/562/739/912 samples at 48 kHz, in a 1024-sample line (4096 at 192 kHz).
- The 200 Hz high-pass keeps the reflections from building up boom and combing the bass.
- The line is always fed; the taps are skipped while room is 0 and not ramping.
- The reflections are direction-independent (one mono bus) and are not head-shadowed.

#### 8.3.6 Renderer B: HRIR convolution

- **Validation** in `prepare()`:
  - the layout enum is valid and `length ≥ 1`;
  - left and right entry counts are equal and match the layout (the 5.1/7.1 LFE entry may be present, and is then ignored, or omitted);
  - every IR holds at least `min(length, kMaxHrirTaps)` taps, and those taps are all finite.

  Sets longer than `kMaxHrirTaps = 1024` taps (21.3 ms at 48 kHz, enough for any anechoic HRIR) are truncated to 1024. The last `kHrirTruncationFade = 64` kept taps are then multiplied by a half-cosine fade-out, `w = 0.5 + 0.5 · cos(π (k − 960 + 1) / 64)` for k = 960 … 1023, which reaches exactly 0 at the last tap. A cut tail therefore does not end in a step. Sets of 1024 taps or fewer are used unchanged.
- **Selection.** The HRIR renderer runs only if the set's `sampleRate` equals the session rate within 0.5 Hz and its layout equals the running layout. Otherwise, or for a malformed set, the parametric renderer runs silently. A set is never resampled here.
- **Convolution** is direct-form:
  - each input sample is written twice into a mirrored history of 2L samples, so the newest L samples are always contiguous;
  - each output is one dot product per ear with the time-reversed IR;
  - four partial sums per ear use a fixed summation order, which is SIMD-friendly and block-size independent.
- The rear shelf, ITD and head shadow are **not** applied, because measured responses carry those cues. LFE, reflections and trim are.
- `setHrirSet()` is structural: a set handed over after `prepare()` is ignored until the next `prepare()`.
- **Status:** no host loads HRIR sets today. The chain always uses renderer A. The SOFA loader, background resampling and a uniformly-partitioned FFT convolver are **roadmap** (`docs/07-roadmap.md` item 2.6).

#### 8.3.7 Output level, channel handling, robustness

- **Output level.** `ear × 0.70794578` (`10^(−3/20)`) × swap fade.
  - There is **no normalisation by speaker count.** Fully correlated full-scale content on every channel therefore exceeds 0 dBFS before the chain's limiter.
  - Measured with 7.1 at the defaults, 48 kHz:
    - 1.0 DC on all 8 channels settles at 5.66 (8 × 0.708, **+15.1 dBFS**; 5.98 peak at the onset);
    - 0 dBFS 60 Hz sine on all 8 reaches +14.3 dBFS;
    - correlated white noise of peak 1 on all 8 reaches +10.1 dBFS;
    - a single FC channel at 0 dBFS, 1 kHz, gives −4.0 / −3.7 dB at L / R. The asymmetry comes from the reflections, whose per-ear taps differ.
  - The maximizer's true-peak limiter downstream absorbs this. Real game mixes are far from fully correlated full scale.
- **Channel handling.**
  - Processing is in place: every input is read into scratch accumulators before channels 0/1 are written.
  - Inputs beyond the layout are ignored and cleared.
  - Layout channels missing from a block are silent, and their state is cleared once.
  - A 1-channel host bus receives `(L + R)/2`.
- **Robustness.** `prepare()` maps a non-finite or non-positive rate to 48 kHz and clamps the rate to 8–768 kHz. At the end of every segment, IIR states below 1e−15 or non-finite are zeroed. The delay lines are FIR, so after a NaN the module recovers by itself within one line length. For the parametric renderer the longest is the reflection line: 1024 samples at 48 kHz and 4096 at 192 kHz. An HRIR history needs its IR length L.

### 8.4 Parameters

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Headphone Virtualizer | `virt.on` | off/on | on | toggle | chain chooses virtualiser (on) or BS.775 downmix (off) for strips with more than 2 channels, with a 20 ms crossfade on every toggle; no effect on stereo strips |
| Front Speaker Angle | `virt.front` | 22 … 45 | 30 | ° | FL/FR at ∓front |
| Side Speaker Angle | `virt.side` | 80 … 120 | 100 | ° | SL/SR at ∓side |
| Rear Speaker Angle | `virt.rear` | 120 … 165 | 145 | ° | BL/BR at ∓rear (7.1) |
| Head Radius | `virt.headRadius` | 70 … 105 | 87.5 | mm | a in the ITD and shadow models (personalisation) |
| Room | `virt.room` | 0 … 1 | 0.15 | % | early-reflection level |
| LFE Level | `virt.lfe` | −20 … +10 | 0 | dB | LFE gain to both ears |
| (layout) | — | Stereo, 5.1, 7.1 | from the strip | — | the chain sets it from the strip channel count: ≥ 8 → 7.1, 6–7 → 5.1, 3–5 → stereo |
| (HRIR set) | API `setHrirSet()` | — | none | — | structural; renderer B (not wired to any host yet) |

Module sanitising: NaN takes the default; ±inf clamps to the range edge; an invalid layout enum becomes 7.1. `speakerAzimuthDeg()` returns NaN for the LFE and for channels outside the layout.

### 8.5 Smoothing & click-freeness

- **Control rate** `fs / 16`, counted in absolute stream time. While the module is *busy*:
  - each tick advances one-pole smoothers (τ = 30 ms, coefficient computed for the control rate) for the three angles and the head radius;
  - it redesigns every speaker's delays, Lagrange taps and shadow coefficients;
  - across the following 16 samples, delays (hence Lagrange taps), shadow `b0/b1/a1` and the six shelf coefficients are interpolated per sample and land exactly on the new design. Interpolating a1 between two stable first-order poles keeps the pole inside the unit circle.
- **Rear-shelf gain:** its own one-pole (10 ms, control rate) in dB.
- **Room and LFE levels:** linear per-sample ramps over 20 ms.
- **Layout change** (discrete: channel meaning and possibly renderer):

```mermaid
stateDiagram-v2
    [*] --> Running
    Running --> FadingOut: layout target differs
    FadingOut --> Running: target reverted during fade-out (fades back in)
    FadingOut --> Swap: fade reaches exactly 0 (on a tick)
    Swap --> PreRoll: roles, renderer, geometry at targets, per-channel state cleared
    PreRoll --> Swap: another layout change (still silent)
    PreRoll --> FadingIn: pre-roll elapsed
    FadingIn --> Running: fade reaches exactly 1
```

- The fade is `16 · round(5 ms · fs / 16)` samples: 224 at 44.1 kHz, 240 at 48, 480 at 96 and 960 at 192 kHz.
- The pre-roll is `16 · ceil(2 ms · fs / 16)` for the parametric renderer (96 samples at 44.1/48 kHz). For an HRIR it is `max(that, 16 · ceil(min(L, 10 ms · fs) / 16))`. It lets the cleared ITD lines and HRIR histories fill before anything is heard; fading in over an empty path would make the signal arrive as a step at non-zero gain.
- The reflection line and its filters are *not* cleared at a swap, so reflections of content common to both layouts stay continuous.
- The gap is therefore about 12 ms (parametric) to about 20 ms (long HRIR).
- The chain never changes the layout without re-preparing, so in practice this path is exercised only through the API.
- With nothing busy, a block is one segment; the output is sample-identical either way (bit-exact under random 1–4096 partitions with automation).

### 8.6 Latency & CPU

- **Latency: 0** (test *zero latency - near-ear and HRIR paths respond at sample 0*). The Woodworth path delay of up to (a/c)(1 + π/2) is part of the binaural cue.
- CPU, indicative, 7.1 in → binaural out, 48 kHz, 512-sample blocks:

| Configuration | ns / sample frame | % core |
|---|---|---|
| parametric, room 0.15, steady geometry | 78–80 | 0.38 % |
| HRIR direct-form, 128 taps | 467–478 | 2.3 % |
| HRIR 256 taps | 838–844 | 4.0 % |
| HRIR 512 taps | ≈ 1 700 | 8.2 % |
| HRIR 1024 taps | 3 340–3 450 | 16.5 % |

HRIR cost is O(L) per speaker, ear and sample: 7 speakers × 2 ears × L multiply-adds. Direct form is practical up to about **512–1024 taps**, which is why `kMaxHrirTaps` is 1024: the worst accepted set costs about 16.5 % of a core at 7.1/48 kHz. Long BRIRs (a room tail of hundreds of ms) are truncated to that length and would need the partitioned FFT convolver (roadmap).

During a `virt.on` crossfade the chain runs both the virtualiser and the BS.775 downmix for 20 ms.

### 8.7 Gaming vs Music usage

- **Gaming.** The Game strip is 7.1, so with `virt.on` (default on) games are rendered binaurally.
  - The chain's *binaural lock* then fixes width 1, space 0 and crossfeed 0 in the spatializer (section 7); positional focus stays available.
  - Presets for 7.1 headphone play (`gaming-7-1-headphone-surround`) rely on it.
  - Device profiles warn against stacking a headset's own virtual surround, or Windows Sonic, with this module (`adviceFor()`, [`core/src/engine/DeviceProfiles.cpp`](../core/src/engine/DeviceProfiles.cpp)).
- **Music.** Music strips are stereo, so the module does not run. No macro targets `virt.*` in either mode.

### 8.8 Tests that prove it (`tests/test_virtualizer.cpp`)

- **Geometry and cues:**
  - *HeadphoneVirtualizer: speaker azimuth mapping for stereo, 5.1 and 7.1*
  - *HeadphoneVirtualizer: side-left source is louder at 4 kHz and leads at the left ear by the Woodworth ITD* (ILD 6–25 dB at 4 kHz; lag within ±2 samples of the model)
  - *HeadphoneVirtualizer: ITD follows the head radius and the sample rate*
  - *HeadphoneVirtualizer: centre speaker reaches both ears identically*
  - *HeadphoneVirtualizer: rear cue - BL is darker than FL at the left ear*
  - *HeadphoneVirtualizer: LFE reaches both ears equally, low-passed, at lfeGainDb*
  - *HeadphoneVirtualizer: stereo layout renders two mirrored virtual speakers*
  - *HeadphoneVirtualizer: room amount adds reflections 4-19 ms after the direct sound*
- **HRIR renderer:**
  - *HeadphoneVirtualizer: HRIR renderer produces exactly the expected delayed/scaled copies*
  - *HeadphoneVirtualizer: HRIR renderer matches a reference convolution for dense responses*
  - *HeadphoneVirtualizer: HRIR set with another sample rate or layout falls back to the parametric renderer*
  - *HeadphoneVirtualizer: HRIRs longer than 1024 taps are truncated with a half-cosine fade-out*
- **Contract, RT safety, timing:**
  - *HeadphoneVirtualizer: channels >= 2 are zero after processing*
  - *HeadphoneVirtualizer: no allocation in reset / setParams / process*
  - *HeadphoneVirtualizer: robustness - silence, DC, full-scale noise, impulses, extreme params, all rates*
  - *HeadphoneVirtualizer: fewer channels than the layout, mono blocks and odd blocks are safe*
  - *HeadphoneVirtualizer: output is independent of the host block size*
  - *HeadphoneVirtualizer: zero latency - near-ear and HRIR paths respond at sample 0*
  - *HeadphoneVirtualizer: angle / head-radius changes are click-free and land on the target design*
  - *HeadphoneVirtualizer: layout changes fade out / swap / fade in without a click*
- **Adversarial:**
  - *HeadphoneVirtualizer [adversarial]: centre and ear-axis paths match the header model sample by sample* (independent Lagrange + bilinear reference, 1e−6)
  - *HeadphoneVirtualizer [adversarial]: ILD matches the Brown-Duda response at every rate*
  - *HeadphoneVirtualizer [adversarial]: largest ITD at 192 kHz fits the delay line without wrapping*
  - *HeadphoneVirtualizer [adversarial]: bit-exact under random block partitions with continuous automation*
  - *HeadphoneVirtualizer [adversarial]: continuous angle automation on every block stays click-free*
  - *HeadphoneVirtualizer [adversarial]: layout toggled every block never clicks and settles on the final layout*
  - *HeadphoneVirtualizer [adversarial]: 7.1 fed only FL/FR equals the stereo layout exactly*
  - *HeadphoneVirtualizer [adversarial]: a NaN / Inf input sample does not latch the module*
  - *HeadphoneVirtualizer [adversarial]: re-prepare switches the renderer with the session rate*
  - *HeadphoneVirtualizer [adversarial]: absurd sample rates in prepare() do not hang or produce NaN*
  - *HeadphoneVirtualizer [adversarial]: tiny inputs decay to exact zero at every rate (no subnormal crawl)*
  - *HeadphoneVirtualizer [adversarial]: switching renderer (HRIR 7.1 -> parametric stereo) is click-free*
- **Chain** (`tests/test_engine.cpp`):
  - *Chain: 7.1 input is virtualised (or downmixed) to stereo; extra channels cleared*
  - *Chain: toggling the virtualiser on a 7.1 strip crossfades (no step in the output)* (off at 1.07 s and on at 2.13 s: the largest sample-to-sample step around each switch stays ≤ 1.25 × the steady-state maximum)

### 8.9 Known limitations

- **HRIR renderer is direct-form**, capped at 1024 taps (see the CPU table). Longer sets, such as BRIRs with a room tail, lose everything after 21 ms (48 kHz) apart from the 64-tap fade. No host loads HRIRs yet, and there is no public accessor reporting which renderer is active.
- **No speaker-count normalisation.** A surround-to-binaural fold of fully correlated full-scale content can exceed 0 dBFS by up to about 15 dB before the limiter (§8.3.7).
- **Switching `virt.on` changes level and image over 20 ms.** The chain crossfades the two folds, so there is no step, but they differ in level (−3 dB trim vs the BS.775 0.7071 weights) and in timing (ITD, head shadow). The switch is audible as a 20 ms glide rather than as a click.
- **Lagrange top-octave loss:** up to −3.25 dB at 16 kHz at half-sample delays (48 kHz).
- **Model simplifications:**
  - a spherical head (no pinna notches, no elevation);
  - the rear cue switches as the side angle crosses 90° (with a 10 ms glide) instead of blending with angle;
  - reflections are direction-independent and unshadowed.
- **Room and LFE also apply in HRIR mode.** Users of BRIRs, which already contain a room, should set room to 0.

---

## 9. Look-ahead compressor (downward + upward)

Sources: [`core/include/flub/dsp/Compressor.h`](../core/include/flub/dsp/Compressor.h), [`core/src/dsp/Compressor.cpp`](../core/src/dsp/Compressor.cpp).

### 9.1 Purpose

A linked, look-ahead, feed-forward compressor that combines two curves:
- a **downward** curve for level control;
- an optional **upward** curve that lifts quiet detail ("environment detail", night mode) without lifting the noise floor.

Detection is linked across all channels, which is mandatory for games: unlinked gain on a game mix moves sounds between the ears. Everything runs per sample, so the output is bit-exact for any host block size.

### 9.2 Signal flow

```
 x (all channels) ─────────────────────────────────────────────► delay L (look-ahead) ─► × F ─► y
   │                                                                                      ▲
   └─► per channel: d = x + w · (HP_sc(x) − x)        HP_sc: 12 dB/oct Butterworth SVF    │
         ─► p = max_c |d|                              (linked, NaN ignored)               │
         ─► two-bucket peak hold (B+1 … 2B samples) ─► L = 20 log10(held) ∈ [−160, +100]   │
         ─► static curve  g_t = gDown(L) + gUp(L)                                          │
         ─► attack / release in dB (double; program-dependent release) ─► g               │
         ─► F = (1 − mix) + mix · 10^((g + makeup) / 20) ───────────────────────────────────┘
```

The gain computed from the detector at sample n is applied to `x[n − L]`: the gain starts moving L samples before the event plays.

### 9.3 Algorithm & maths as implemented

**Static curves.** With slope `s = 1 − 1/R`, knee width W, `o = L − T`:

```
downward (Giannoulis/Massberg/Reiss 2012 soft knee; W = 0 is a hard knee):
   2o < −W        : gDown = 0
   |2o| ≤ W       : gDown = −s (o + W/2)² / (2W)
   2o > W         : gDown = −s · o
upward (only when upMax > 0 and L < upT), s_u = 1 − 1/upR:
   gUp = min(upMax, (upT − L) · s_u) · clamp((L − upFloor) / 12 dB, 0, 1)
target g_t = gDown + gUp                            (computeGainDb() returns exactly this)
```

The knee joins with matching value and slope at `T ± W/2`. The upward lift fades out linearly over the 12 dB above `upFloor`, so silence, hiss and room tone are never pulled up. Worked values:

| Detector peak | −30 | −21 | −18 | −15 | −10 | 0 dBFS |
|---|---|---|---|---|---|---|
| gDown, defaults (T −18, R 2.5, W 6) | 0 | 0 | −0.45 | −1.80 | −4.80 | −10.80 dB |

| Detector peak | −40 | −50 | −60 | −63 | −69 | −75 dBFS |
|---|---|---|---|---|---|---|
| gUp (upT −45, upR 2, upMax 6, floor −75) | 0 | +2.5 | +6.0 (capped) | +6.0 | +3.0 (taper ½) | 0 dB |

With the default upward threshold, ratio and floor (−45 dBFS, 2:1, −75 dBFS), the uncapped lift peaks at **+9 dB at −63 dBFS**: `(−45 + 63) · 0.5 = 9`, taper 1. An `upMax` above 9 dB therefore only matters once the upward threshold, ratio or floor are moved away from their defaults.

**Peak hold.** Two alternating buckets of B samples, `held = max(current, previous)`, which always covers the last B+1 to 2B samples:

```
B = max(1, L, round(fs / (2 f_low))),   f_low = max(20 Hz, scHp / 2) with the sidechain HP on, 20 Hz with it off
48 kHz: HP 80 Hz → B = 600 (12.5 ms);  HP off → 1200 (25 ms);  HP 300 Hz → 160 (3.3 ms), raised to L if L is longer
```

- A steady tone always has a waveform peak inside the window, so the level does not ripple at twice the signal frequency and the gain does not modulate the bass. A −8 dBFS tone with T −20, R 4 settles at −9.0 dB, the static-curve value.
- `B ≥ L` keeps every sample still inside the look-ahead delay held, so the release can never start before the loudest delayed sample has played.
- The side effect is that recovery = hold (B … 2B) + release.

**Level.** `L = min(20 log10(held), +100 dB)`, floored at −160 dB. The +100 dBFS clamp keeps the gain finite for ±Inf input. It is recomputed only when `held` changes, and the curve is re-evaluated only when L or the smoothed curve moves.

**Gain smoothing** (the `GainSmoother` law, §0.9, implemented inline in double precision):

```
g ← g_t + c · (g − g_t),   c = exp(−1 / (τ · fs))                        state and coefficients in double
  τ = attack   when g_t < g   (gain falling: level rising; also removes an upward lift)
  τ = release  when g_t > g   (with autoRelease and g < 0 dB: τ = release · (0.25 + 0.75 σ))
  |g − g_t| < 1e−5 dB  →  g = g_t                                         steady state costs no exp
```

The double state matters. A float one-pole with a 2000 ms release at 192 kHz stalled 0.74 dB short of its target (review finding, test *the gain lands on the static curve even with the slowest times at 192 kHz*).

**Program-dependent (auto) release.**
- An *episode* runs while the held target asks for more than 0.5 dB of downward reduction.
- `onsetGain` is the linear detector peak at which the smoothed downward curve reaches −0.5 dB. `reductionOnsetGain()` computes it in closed form (inside the knee `x = T − W/2 + sqrt(2·0.5·W/s)`, otherwise `x = T + 0.5/s`).
- `loudRun` is the episode-relative index of the last sample whose *instantaneous* linked peak exceeded `onsetGain`. The hold is therefore excluded exactly from the persistence.
- `σ = max(σ, min(1, loudRun / (0.1 s · fs)))`, so reduction that persisted for 100 ms or more releases at `releaseMs`, and a 5 ms transient at about `releaseMs / 4`.
- While the gain is still below −0.5 dB, σ is held, so dense material stays on the slow release (no pumping). Once the gain has recovered, σ falls linearly to 0 over 100 ms.
- The upward lift (g > 0 dB) always rises at `releaseMs`, so short gaps are not pumped up.

**Output.** One delay line serves as both the wet input and the dry path, so parallel compression is phase-aligned and collapses to one factor:

```
y = x[n − L] · ((1 − mix) + mix · 10^((g + makeup) / 20))    exactly 1 at mix 0; exactly G at mix 1
makeup = manual makeupDb, or with autoMakeup: clamp(−gDown(0 dBFS) / 2, 0, +24) dB     (defaults: +5.4 dB)
```

`F` (one `exp`) is recomputed only when `g + makeup` or mix changes.

**Meters.** `getGainReductionDb() = min(0, block minimum of g)` and `getUpwardGainDb() = max(0, block maximum of g)`. Both show the compressor's own net gain, excluding makeup and independent of mix.

**Housekeeping,** every 64 samples of stream time:
- sidechain SVF states below 1e−20 are flushed;
- a non-finite HP state is reset *per channel*, so a NaN on one channel does not disturb the others;
- a non-finite gain returns to 0 dB.

Prepared channels missing from a narrower block are fed zeros through their delay lines and their HP states are reset, so a rejoining channel starts from silence rather than stale audio.

### 9.4 Parameters

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Compressor | `comp.on` | off/on | **off** | toggle | module bypass (Gaming macros engage it, upward only unless a ratio is set: §9.7) |
| Threshold | `comp.threshold` | −60 … 0 | −18 | dBFS | downward threshold T (detector peak) |
| Ratio | `comp.ratio` | 1 … 20 | 2.5 | ratio | downward slope `1 − 1/R` |
| Knee | `comp.knee` | 0 … 24 | 6 | dB | soft-knee width W (0 = hard) |
| Attack | `comp.attack` | 0.1 … 200 | 10 | ms | one-pole τ for a falling gain |
| Release | `comp.release` | 10 … 2000 | 120 | ms | one-pole τ for a rising gain (after the hold) |
| Auto Release | `comp.autoRelease` | off/on | off | toggle | release scales `0.25 … 1 × releaseMs` with persistence |
| Makeup | `comp.makeup` | −12 … +24 | 0 | dB | manual makeup |
| Auto Makeup | `comp.autoMakeup` | off/on | off | toggle | makeup = half the reduction at 0 dBFS (≤ +24 dB) |
| Sidechain High-Pass | `comp.scHp` | 0 … 300 | 80 | Hz | 0 = off; otherwise clamped to 20 … 300 Hz by the module |
| Mix | `comp.mix` | 0 … 1 | 1 | % | phase-aligned parallel compression |
| Upward Threshold | `comp.upThreshold` | −80 … −10 | −45 | dBFS | below it, quiet material is lifted |
| Upward Ratio | `comp.upRatio` | 1 … 10 | 2 | ratio | lift slope `1 − 1/upR` |
| Upward Max Gain | `comp.upMax` | 0 … 18 | 0 | dB | cap on the lift; 0 = upward off |
| Upward Floor | `comp.upFloor` | −100 … −40 | −75 | dBFS | lift fades to 0 over the 12 dB above it |
| (look-ahead) | `latency.profile` | 0.5 / 1 / 3 ms | 1 ms (Balanced) | ms | structural; module accepts 0 … 10 ms (NaN → 0) |

Module sanitising: out-of-range values clamp, and a non-finite field keeps its last valid value. An unchanged parameter set returns early.

### 9.5 Smoothing & click-freeness

| Change | Mechanism | Time |
|---|---|---|
| threshold, knee, slope (ratio), upward threshold/slope/max/floor, makeup (manual or auto), mix | per-sample one-poles (ratios smoothed as slopes, so the curve glides evenly); a step that no longer moves the float value lands on the target (local `glide()`), so glides really finish | τ = 20 ms (≈ 280 ms to land) |
| sidechain HP corner | one-pole on ln(Hz); SVF coefficients refreshed every 16 samples and on landing | 20 ms |
| sidechain HP on/off | detector input crossfaded raw ↔ high-passed; a re-enabled HP restarts from rest at the new corner under the fade | 20 ms linear |
| attack / release times | coefficients only; the gain state stays continuous | immediate |
| module on/off | `ModuleSlot` crossfade against the latency-delayed dry path; re-activation resets and pre-rolls for L + 64 samples | 20 ms |

### 9.6 Latency & CPU

- **Latency = L = round(lookahead · fs)**, fixed at `prepare()`. Per profile: 3 ms (144 samples at 48 kHz, 132 at 44.1 kHz) in Quality, 1 ms (48 / 44) in Balanced, 0.5 ms (24 / 22) in Low Latency. Test *latencySamples() = round(lookahead \* fs) and a quiet impulse arrives exactly that late* checks this.
- **The look-ahead is partial anticipation, not a ceiling guarantee.** With the default 10 ms attack and a 1 ms look-ahead, the gain has covered `1 − e^(−0.1)` ≈ 9.5 % of a step when the transient plays. A ceiling is the limiter's job (section 10).
- CPU (indicative): **22–23 ns** per stereo sample with the downward and upward curves active (0.11 % of a core), independent of the look-ahead. The implementer measured about 12.5 ns for steady or silent input. Transcendentals run only when the held level changes (`log10`), while the gain or makeup/mix moves (`exp`), when σ changes during a release (`exp`), and during a corner glide (`tan` every 16 samples).

### 9.7 Gaming vs Music usage

- **Gaming** (all ungoverned):
  - **Boost Intensity** engages `comp.on` from about 6 % (smoothstep 5–7 %) and adds upward max +5 dB (10–70 %).
  - **Footsteps** engages it and adds upward max +3 dB (30–100 %).
  - **Detail** engages it and adds upward max +8 dB (0–100 %).
  - Together they reach +16 dB (store range 0 … 18). With the default upward curve the lift is capped by the curve itself at +9 dB (above).
  - **A macro-engaged compressor is upward-only.** When only these macros switched the module on (base `comp.on` off) and `comp.ratio` is still at its default 2.5, `ProcessingChain::applyParameters()` sets the effective ratio to **1:1**. The downward slope `1 − 1/R` is then 0, so gunshots and explosions keep their dynamics while quiet cues are lifted. Before this rule the default downward curve (−18 dBFS, 2.5:1, 6 dB knee) took about 4.8 dB off a −10 dBFS tone at Footsteps 100 %. A preset that sets a ratio keeps it (the Gaming factory presets that engage the compressor choose 1:1 to 3:1), and so does a compressor the user switched on. The effective value shows the 1:1 (§14.1). Test: *Gaming: a compressor switched on only by a macro is upward-only - loud sounds keep their dynamics unless a ratio was chosen* (`tests/test_modes.cpp`: with only Footsteps engaging the compressor, the effective ratio reads 1:1 and a −10 dBFS 1 kHz tone stays within 0.2 dB of the macro-off level; with a stored 1.5:1 it comes out more than 1 dB lower). A compressor the user switched on is rendered too: its effective ratio stays at the default 2.5 and it takes more than 1 dB off the loud tone.
  - Where a downward ratio is in force, the 80 Hz sidechain high-pass keeps explosions from ducking everything.
  - The Low Latency profile uses a 0.5 ms look-ahead.
- **Music.** No Music macro touches the compressor, and `comp.on` defaults to off. It is a user/preset tool, for example dynamic genres or night listening via the upward curve.

### 9.8 Tests that prove it (`tests/test_compressor.cpp`)

- **Static curves:**
  - *Compressor: static curve is 0 below the knee, continuous inside it and has slope 1/R above it*
  - *Compressor: upward curve lifts below its threshold, caps at upMax and tapers to 0 at the floor*
  - *Compressor: static curve sanitises out-of-range and non-finite input*
  - *Compressor: 1 kHz tone at -8 dBFS, threshold -20, ratio 4, hard knee settles at -9 dB*
  - *Compressor: upward compression lifts a -50 dBFS tone by ~5 dB and leaves the floor alone*
  - *Compressor: makeup is manual, or auto = half the reduction at 0 dBFS (knee included)*
- **Detection and timing:**
  - *Compressor: linked detection gives both channels identical gain when only one is loud*
  - *Compressor: sidechain high-pass stops a loud 30 Hz tone from causing much reduction*
  - *Compressor: look-ahead attenuates an isolated burst from its very first sample*
  - *Compressor: look-ahead withdraws the upward lift before a loud transient arrives*
  - *Compressor: auto release recovers faster after a short burst than after a long one*
  - *Compressor: mix 0 is exactly the input delayed by the latency; mix 0.5 blends aligned paths*
  - *Compressor: latencySamples() = round(lookahead \* fs) and a quiet impulse arrives exactly that late*
- **Click-freeness, RT safety, robustness:**
  - *Compressor: parameter changes during a loud tone are click-free*
  - *Compressor: reset, setParams and process do not allocate*
  - *Compressor: silence, DC, full-scale noise, impulses and extreme settings stay finite and bounded*
  - *Compressor: recovers from NaN / Inf input samples*
  - *Compressor: output is independent of the host block size (1, 7, 64, 512)*
  - *Compressor: 1..8 channels, all linked, and blocks narrower than the prepared width*
- **Adversarial:**
  - *Compressor [adversarial]: the gain lands on the static curve even with the slowest times at 192 kHz*
  - *Compressor [adversarial]: after a parameter glide the output matches a compressor set up with the final values*
  - *Compressor [adversarial]: attack and release time constants are exact and sample-rate independent* (hold 24–51 ms with the HP off; attack 5 ms ± 0.05; release 100 ms ± 1; at 44.1/48/96/192 kHz)
  - *Compressor [adversarial]: auto release after a 5 ms transient runs at ~releaseMs/4, whatever the hold phase*
  - *Compressor [adversarial]: a channel that rejoins after narrower blocks does not replay stale audio*
  - *Compressor [adversarial]: a NaN on one channel does not disturb the detector of the others*
  - *Compressor [adversarial]: the hold covers the full 10 ms look-ahead even with a 300 Hz sidechain corner*
  - *Compressor [adversarial]: random automation, block sizes and signals never produce NaN/Inf or out-of-range meters*
  - *Compressor [adversarial]: parameter changes at the same sample positions give identical output for any block split*

### 9.9 Known limitations

- **The peak hold is not user-adjustable.** Release begins only after B … 2B samples: 12.5–25 ms with the default 80 Hz sidechain HP, 25–50 ms with it off, and never less than the look-ahead. The GUI should label the release "after hold".
- **Peak detection on the high-passed sidechain** does not bound the unfiltered audio peak, so this is not a limiter. With the HP at fc, content below fc/2 (already ≥ 12 dB down) can still ripple the held level slightly; for example, a 30 Hz tone with HP 80 Hz has a half period (16.7 ms) longer than the bucket (12.5 ms).
- **Auto release** scales only the recovery from downward reduction. After a long reduction, σ stays 1 until the gain is back above −0.5 dB, so a short transient that follows immediately still releases slowly.
- **Auto makeup is capped at +24 dB.** The raw formula would give up to +28.5 dB at T −60, R 20.
- **Overlapping curves.** Where the downward and upward regions overlap (`upThreshold > threshold`), the meters show the net gain rather than the two parts.
- **Non-finite input samples** pass through the delay: that sample's output is non-finite. A ±Inf reads as a +100 dBFS peak, which causes deep reduction for hold + release. `ProcessingChain` drops non-finite blocks before they reach any module.

---

## 10. True-peak limiter

Sources: [`core/include/flub/dsp/TruePeakLimiter.h`](../core/include/flub/dsp/TruePeakLimiter.h), [`core/src/dsp/TruePeakLimiter.cpp`](../core/src/dsp/TruePeakLimiter.cpp), [`core/include/flub/dsp/TruePeakDetector.h`](../core/include/flub/dsp/TruePeakDetector.h).

### 10.1 Purpose

The limiter is the **only** stage that guarantees the ceiling (rule 5 of §1.1). It is used three times:
- inside every strip's `LoudnessMaximizer` (section 11), at `max.ceiling`;
- on every strip's global-bypass reference while bypass is engaged (`dryLimiter` in `ProcessingChain`, at `max.ceiling`, 1 ms look-ahead, 80 ms auto release; §14.5);
- as the desktop app's master limiter in `MixEngine` across all strips (section 14).

Its guarantee:
- the output never exceeds the ceiling in **sample peak**, for any input; this is proven below and enforced by a counted last-resort clamp;
- it stays within about **0.1 dB of the ceiling in true (inter-sample) peak** for content up to about 0.45 fs;
- it has no gain overshoot, is linked across channels and runs strictly per sample.

### 10.2 Signal flow

```
 x[n] (all channels) ─► RefinedPeakDetector (4× polyphase, 40 taps/phase, Kaiser β 5 = the meter's taps, parabolic refinement; delay D = 20)
        ─► p[n] = max over channels                      (|x| and D = 0 with true-peak detection off)
        ─► r[n] = p[n] > thr ? thr / p[n] : 1            thr = ceiling · 10^(−0.05/20)   (0.05 dB margin)
        ─► m[n] = min(r[n−L−Kh−1 … n])                   sliding minimum, L + Kh + 2 samples (monotonic deque)
        ─► a[n] = mean(m[n−L+Kh … n])                    box filter, L − Kh + 1 samples (double running sum)
        ─► g[n] = a[n] ≤ g[n−1] ? a[n] : a[n] + c · (g[n−1] − a[n])     release one-pole (double)
 x[n] ─► delay L + D ─► × g[n] ─► safety clamp |y| ≤ ceiling in force when r[n−L] was computed (counted) ─► y[n]
```

`L = round(lookahead · fs)` and `D = TruePeakDetector::kDelay = 20`. `Kh` is the **gain hold**: `Kh = min(kTruePeakHold = 8, ⌊L / 3⌋)` with true-peak detection on, and 0 in sample-peak mode (§10.3.2).

### 10.3 Algorithm & maths as implemented

#### 10.3.1 The limiter's own true-peak detector

`RefinedPeakDetector` (private to the limiter) uses **exactly the meter's interpolator**: its taps come from the shared design function `TruePeakDetector::designPhaseTaps()` of §0.7 (161-tap Kaiser-windowed sinc, β = `kKaiserBeta` = 5.0, 40 taps per phase, `kDelay = 20`, phases 1…3 at unity DC gain, phase 0 = the delayed sample `x[n − 20]` itself), so the limiter and the meters can never read different peaks.

Per call it forms |z| on the 4× grid:

```
a0 = n−D−¼ (previous call's phase 3)   a1 = n−D   a2 = n−D+¼   a3 = n−D+½   a4 = n−D+¾   a5 = n−D+1 (= x[n−19], already in the history)
peak = max(a1 … a4, and for every local maximum a_i (i = 1…4, a_i ≥ neighbours, den < 0):
                     a_i − (a_{i−1} − a_{i+1})² / (8 · den),  den = a_{i−1} − 2 a_i + a_{i+1})
```

The reading covers positions `[n − D − 1/8, n − D + 7/8)`. The parabolic vertex can only raise the estimate, and it costs no extra delay because `a5` is already in the history.

**Why refinement.** The plain maximum of a 4× grid under-reads a peak that falls between grid points by up to `cos(π f / (4 fs))` for a sine at f: −0.03 dB at 0.1 fs, −0.17 dB at 0.25 fs, −0.24 dB at 0.3 fs and −0.44 dB at 0.4 fs. That is more than the 0.05 dB margin. The header gives the refined residual as below 0.03 dB; §10.6 has the end-to-end measurement.

**Why the shared design.** An earlier revision gave the limiter its own Kaiser β 8 taps. They rolled off sooner near Nyquist than the meters' β 5 taps, so the limiter read peaks lower than the meters did. Together with the missing gain hold (§10.3.2), this let factory presets at full macros reach −0.62 dBTP on the meter for a −1 dBTP ceiling (figure recorded in the commit message of the change, 81f6b72, together with −1.04 dBTP after it). There is now one design, `TruePeakDetector::designPhaseTaps()`, used by both.

**Interpolator accuracy.** The phase responses are those of §0.7: flat within −0.02 / +0.04 dB up to 0.4535 fs (20 kHz at 44.1 kHz), with an error against an ideal fractional delay below −54 dB up to 0.4 fs. The small over-read near 0.45 fs is conservative for a limiter. The 0.05 dB margin absorbs the in-band error, and the measurements in §10.6 show the net result.

#### 10.3.2 Envelope: sliding minimum + box filter + gain hold, and why it never overshoots

Definitions: `n` is the sample index. `r[n] ≤ 1` is the gain that sample n's reading requires. The audio delay is `L + D`, so the output at time n is `y[n] = g[n] · x[n − L − D]`. The sample `x[n − L − D]` is the phase-0 point of the reading `p[n − L]`. `Kh` is the gain hold of §10.2.

```
m[k] = min{ r[j] : k − L − Kh − 1 ≤ j ≤ k }                 (window of L + Kh + 2)
a[n] = (1 / (L − Kh + 1)) · Σ_{k = n−L+Kh}^{n} m[k]          (window of L − Kh + 1)
Kh   = min(8, ⌊L / 3⌋) with true-peak detection, 0 in sample-peak mode
```

| Look-ahead | L @ 48 kHz | Kh | attack ramp `L − Kh + 1` | min window `L + Kh + 2` |
|---|---|---|---|---|
| 0.5 ms (Low Latency; master limiter when every strip is Low Latency) | 24 | 8 | 17 | 34 |
| 1.0 ms (master limiter, other profiles) | 48 | 8 | 41 | 58 |
| 1.5 ms (Balanced) | 72 | 8 | 65 | 82 |
| 2.0 ms (Quality) | 96 | 8 | 89 | 106 |

Kh is 8 for every profile look-ahead at 44.1 to 192 kHz, except 0.5 ms at 44.1 kHz: there L = 22, so Kh = 7 and the ramp is 16 samples. Look-aheads below 3 samples (API only) get Kh = 0.

**Lemma 1 (the envelope is at or below every requirement it must honour).** `a[n] ≤ r[j]` for every `j ∈ [n − L − Kh − 1, n − L + Kh]`.

*Proof.* Take any k in [n − L + Kh, n]. The window of m[k] is [k − L − Kh − 1, k]. Since k ≤ n, `k − L − Kh − 1 ≤ n − L − Kh − 1`, and since k ≥ n − L + Kh, `n − L + Kh ≤ k`. So the whole range [n − L − Kh − 1, n − L + Kh] lies in m[k]'s window, so every term of the mean satisfies `m[k] ≤ r[j]` for every j in it. A mean is at most its largest term. ∎

With Kh = 0 this is `a[n] ≤ min(r[n − L − 1], r[n − L])`. Read the other way round, reading `p[j]` is honoured by every output sample `n ∈ [j + L − Kh, j + L + Kh + 1]`. Those are the 2Kh + 2 output samples that carry `x[j − D − Kh] … x[j − D + Kh + 1]`: the two samples that bracket the reading and Kh more on each side.

**Lemma 2 (the release never undoes it).** `g[n] ≤ a[n]`. If `a[n] ≤ g[n−1]`, then `g[n] = a[n]`. Otherwise `g[n] = a[n] + c (g[n−1] − a[n])` with `c ∈ [0, 1)` and `g[n−1] − a[n] < 0`, so `g[n] < a[n]`. The landing rule (`a − g < 1e−7 ⇒ g = a`) gives equality, never more. ∎

**Theorem (sample peak).** `|y[n]| ≤ thr < ceiling` for every n, in exact arithmetic.

*Proof.* Phase 0 of the detector is the sample itself, so `|x[n − L − D]| ≤ p[n − L]`.
- If `p[n − L] ≤ thr`, then `|y[n]| ≤ g[n] · thr ≤ thr`, because g ≤ 1.
- Otherwise `r[n − L] = thr / p[n − L]`. The index j = n − L lies in the range of Lemma 1 for every Kh ≥ 0, so by Lemmas 1 and 2 `|y[n]| ≤ r[n − L] · p[n − L] = thr`. ∎

In float, the 0.05 dB margin between `thr` and the ceiling covers rounding. The final clamp uses the ceiling that was in force when `r[n − L]` was computed. That ceiling comes from its own per-sample ring of L + 1 entries, separate from the box-filter ring, so a falling ceiling never trips the clamp. The clamp turns NaN into 0 and counts every engagement. The tests require the count to be **0**. In every measurement of §10.6 the sample peak stayed at or below `thr` (−0.0501 dB re ceiling), and the clamp never engaged.

**True peak between samples.** Take the reading `p[j]`, which covers input positions `[j − D − 1/8, j − D + 7/8)`: sample `x[j − D]` and the interval up to the next sample.
- By Lemma 1, both output samples that bracket that interval carry a gain ≤ `r[j]`, and so do Kh further samples on each side.
- For an isolated peak the gain is exactly `r[j]` across all 2Kh + 2 of them (see *attack shape* below). The interpolator that reconstructs the interval weights the samples around it by its kernel. Its central 2Kh + 2 taps (18 at Kh = 8) therefore see a constant gain, and the inter-sample values there are r[j] times those of x.
- Only the kernel's outer taps see the gain ramp, and they carry little weight. That residual, and the interpolator's own error, are what the 0.05 dB margin is for.
- **Why the hold.** Without it (Kh = 0) the attack ramp is still falling across the kernel when the peak arrives. The code comment in `TruePeakLimiter.cpp` records that dense clipped programme then came out up to about 0.05 dB above the ceiling on the same interpolator. With Kh = 8 the worst case it records, on hard-clipped noise and tones at 44.1/48/96 kHz and 0.5–2 ms look-ahead, is 0.04 dB below.
- `Kh ≤ L/3` keeps at least two thirds of the look-ahead for the attack ramp.

The true-peak claim is therefore measured, not proven (§10.6).

**No gain overshoot in the other direction either.** Every m[k] in the mean is at least `min(r[n − 2L − 1 … n])`, because the oldest window in the mean starts at `(n − L + Kh) − L − Kh − 1 = n − 2L − 1`. So the envelope a[n] never dips below the deepest requirement within the preceding 2L + 2 samples: the attack and the hold never reduce more than some sample in that span asked for, and the release only lags the envelope on the way up. The envelope is a FIR (box) of a sliding minimum, so it cannot ring.

**Attack shape.** For an isolated over at `r[n0] = r` (all other r = 1):
- `m = r` on [n0, n0 + L + Kh + 1];
- `a[n0 + j] = 1 − (j + 1)(1 − r)/(L − Kh + 1)` for j = 0 … L − Kh, a linear ramp of L − Kh + 1 samples that reaches r at `n0 + L − Kh`;
- a stays at r for the 2Kh + 2 output samples `n0 + L − Kh … n0 + L + Kh + 1`. They carry `x[n0 − D − Kh] … x[n0 − D + Kh + 1]`: the gain is down Kh samples before the peak sample `x[n0 − D]` plays and stays down until Kh samples after the interval the reading refers to;
- after that a rises linearly and is back at 1 at `n0 + 2L + 2`; g follows at the release rate.

**Implementation.**
- The sliding minimum is a monotonic deque in a power-of-two ring of capacity `nextPow2(L + Kh + 3)`. Indices are `uint32_t` and only their differences are used, so wrap-around is safe. Each entry is pushed and popped once, so it costs O(1) amortised per sample.
- The box filter keeps a float ring of L − Kh + 1 entries with a **double** running sum, re-summed exactly once per ring cycle so it cannot drift.
- `a = sum / (L − Kh + 1)` is a division, so a window full of 1.0 gives exactly 1.0.
- The hold costs no latency: it is taken out of the attack ramp, inside the same look-ahead.

#### 10.3.3 Release

```
slow = exp(−1 / (releaseMs · fs)),   fast = autoRelease ? exp(−1 / (0.2 · releaseMs · fs)) : slow
over      : any sample with r < 1; overs < 25 ms apart belong to one run
runSpan   : time from the first to the latest over of the run
blend     = clamp((runSpan − 25 ms) / 25 ms, 0, 1),   c = fast + (slow − fast) · blend
run reset : no over for 25 ms AND g ≥ 10^(−0.1/20) (within 0.1 dB of unity)
```

- 25 ms is half a period of 20 Hz. A bass note that pokes over the ceiling on every half-cycle therefore counts as *continuous* limiting (slow, low-distortion release) rather than a string of isolated peaks (fast release that would modulate the waveform).
- Runs shorter than 25 ms release at `releaseMs / 5`; runs longer than 50 ms release at `releaseMs`; the coefficient is blended in between.
- The gain state is double: a float one-pole with a long release stalls a few 1e−5 below 1.0. Recovered, the limiter lands exactly on 1.0 and is **bit-transparent** (`y = x` delayed).

#### 10.3.4 Ceiling, telemetry, channels

- **Ceiling.**
  - Linear 50 ms ramp in dB (`ceilingDbS`); the threshold is re-derived per sample while it moves.
  - `setParams()` called before any processing after `prepare()`/`reset()` applies instantly (the chain pushes its parameters right after preparing).
- **Telemetry.**
  - `getGainReductionDb()` = `20 log10` of the block's minimum g, floored at −160 dB.
  - `getSafetyClipCount()` counts clamp engagements since `prepare()` (not reset by `reset()`). The maximizer forwards its limiter's count (`LoudnessMaximizer::getSafetyClipCount()`); the chain publishes it per block as `MeterBus::safetyClipCount`; the desktop master limiter's count is `MixEngine::getMasterSafetyClipCount()`.
- **Channels.** All channels are linked (one gain). Prepared channels missing from a block are fed zeros through the detector and the delay, so a channel that rejoins never replays stale audio.

### 10.4 Parameters

The strip limiter has no keys of its own. `LoudnessMaximizer` passes its parameters through (section 11), and the latency profile sets the structural values.

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Ceiling | `max.ceiling` | −12 … 0 | −1 | dBTP | limiter ceiling (strip) |
| Release | `max.release` | 5 … 1000 | 60 | ms | slow release τ; fast = τ/5 with auto release |
| Auto Release | `max.autoRelease` | off/on | on | toggle | run-span-based fast/slow blend |
| (look-ahead) | `latency.profile` | 2 / 1.5 / 0.5 ms | 1.5 ms (Balanced) | ms | structural; module accepts 0 … 10 ms (non-finite → 1.5 ms) |
| (true-peak detection) | — | on | on in every profile | — | structural; off gives a sample-peak limiter with D = 0 and Kh = 0 |
| (gain hold Kh) | — | 0 … 8 | 8 (7 at 0.5 ms, 44.1 kHz) | samples | derived at `prepare()`: `min(kTruePeakHold = 8, ⌊L/3⌋)` with true-peak detection, else 0; no latency |

Bypass-reference limiter (`ProcessingChain::dryLimiter`, every host, only while global bypass is engaged): ceiling `max.ceiling`, 1 ms look-ahead (capped at chain latency − 20), 80 ms auto release, true peak on; its latency is taken out of the dry-path delay (§14.5).

Master limiter (`MixEngine`, desktop app): ceiling −1 dBTP, capped by the device profile (−2 dBTP Bluetooth A2DP, −3 dBTP hands-free, section 14), 1 ms look-ahead (0.5 ms when every strip runs the Low Latency profile, chosen in `MixEngine::configure()`), 50 ms auto release, true peak on. Module sanitising: out-of-range values clamp; a non-finite value keeps the previous one.

### 10.5 Smoothing & click-freeness

- Ceiling: 50 ms linear ramp in dB; the clamp follows the per-sample ceiling history, so automation while limiting never trips it (tests).
- Release or auto-release changes: coefficients only, immediate.
- Attack: a linear ramp of L − Kh + 1 samples, then a flat hold of 2Kh + 2 samples, and release a one-pole towards the envelope, so the gain is continuous in both directions.
- The limiter is not in a `ModuleSlot` of its own; `max.on` bypasses the whole maximizer (section 11).

### 10.6 Latency & CPU

**Latency = L + D = round(lookahead · fs) + 20** (true peak on). The gain hold Kh does not change it.

| Look-ahead | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz | Used by |
|---|---|---|---|---|---|
| 0.5 ms | 22 + 20 = 42 | 24 + 20 = **44** | 48 + 20 = 68 | 96 + 20 = 116 | Low Latency profile; master limiter (`MixEngine`) when every strip is Low Latency |
| 1.0 ms | 44 + 20 = 64 | 48 + 20 = 68 | 96 + 20 = 116 | 192 + 20 = 212 | master limiter (`MixEngine`), any other mix of profiles; the bypass-reference limiter (`dryLimiter`), inside the chain latency |
| 1.5 ms | 66 + 20 = 86 | 72 + 20 = **92** | 144 + 20 = 164 | 288 + 20 = 308 | Balanced profile (header default) |
| 2.0 ms | 88 + 20 = 108 | 96 + 20 = **116** | 192 + 20 = 212 | 384 + 20 = 404 | Quality profile |

**Measured true-peak accuracy.** Single channel, ceiling −1 dBTP, input peak +12 dBFS, four noise seeds per cell, judged by an ideal (8× zero-padded FFT, sinc) reconstruction, at 44.1, 48 and 96 kHz. Worst value re the ceiling:

| Content | Look-ahead 0.5 ms | 1.5 ms | 2 ms |
|---|---|---|---|
| noise band-limited to 0.39 fs | −0.03 dB | −0.03 dB | −0.03 dB |
| noise band-limited to 0.45 fs (CD-style 20 kHz band at 44.1 kHz) | −0.031 dB (44.1 kHz), −0.026 dB (48), −0.023 dB (96) | −0.023 dB | −0.023 dB |
| raw full-band white noise | +1.35 dB (44.1 kHz), +1.25 dB (48), +1.19 dB (96) | +1.09 dB (44.1), +1.18 dB (48), +0.96 dB (96) | +1.03 dB (44.1), +1.17 dB (48), +0.90 dB (96) |

Sample peak was ≤ −0.0501 dB re ceiling and the safety clamp never engaged in any case. The unit tests assert ≤ +0.1 dB on band-limited programme and ≤ +0.15 dB on full-level noise flat to 0.45 fs (six cases at 44.1 and 48 kHz, which measure −0.05 to −0.03 dB; the test also requires ≥ −0.5 dB, so the limiter cannot pass by muting).

**CPU** (indicative, stereo, while limiting): **150–168 ns** per stereo sample with true-peak detection (≈ 0.8 % of a core), independent of the look-ahead; the range spans the original runs and a re-run after the hold was added. Detection dominates: 3 interpolated phases × 40 taps = 120 multiply-adds per channel-sample. Sample-peak mode costs **11.5 ns** when idle and **20.5–22 ns** while limiting. SIMD for the TP detector is roadmap (`docs/07-roadmap.md` 1.9).

### 10.7 Gaming vs Music usage

The algorithm is identical in both modes; the profiles differ. Competitive Gaming presets use Low Latency: 0.5 ms look-ahead, 44 samples at 48 kHz. Balanced (1.5 ms, 92 samples) is the default. Quality uses 2 ms (116 samples). The shorter look-ahead gives a steeper attack ramp (L − Kh + 1 = 17 samples at 48 kHz, then the 18-sample hold), which is still within the true-peak bound (§10.6).

### 10.8 Tests that prove it (`tests/test_limiter.cpp`)

The file contains an independent true-peak meter (8× FFT sinc reconstruction, shares no code with the limiter).

- **Latency and transparency:**
  - *TruePeakLimiter: independent true-peak meter is accurate (self-test)*
  - *TruePeakLimiter: latencySamples() = lookahead + detector delay, and a quiet impulse arrives exactly that late*
  - *TruePeakLimiter: a -30 dBFS signal passes bit-exactly, only delayed by latencySamples()*
- **Ceiling:**
  - *TruePeakLimiter: ceiling holds in sample peak and independent true peak (<= +0.1 dB) at 44.1/48/96 kHz*
  - *TruePeakLimiter: full-band synthetic signals hold the sample ceiling exactly (true peak bounded loosely)* (≤ +2 dB)
  - *TruePeakLimiter: other ceilings and look-aheads hold the ceiling too*
  - *TruePeakLimiter: a sudden +12 dB step never overshoots and is anticipated by the look-ahead*
  - *TruePeakLimiter [adversarial]: CD-band programme (flat to 0.45 fs) stays within the documented true-peak bound* (≤ +0.15 dB and ≥ −0.5 dB re ceiling, sample peak ≤ ceiling, safety count 0)
  - *TruePeakLimiter [adversarial]: 192 kHz and the latency-profile look-aheads hold the ceiling (sample + true peak)*
- **Envelope and release:**
  - *TruePeakLimiter [adversarial]: gain matches a brute-force model of the header (deque, box filter, attack bound)* (in sample-peak mode, so with Kh = 0; L = 0, 1, 29, 61, 72)
  - *TruePeakLimiter: release follows releaseMs (fixed) and releaseMs / 5 after an isolated peak (auto)*
  - *TruePeakLimiter [adversarial]: after 20 s of dense limiting with a 1 s release the gain lands exactly on 0 dB*
- **Linking and click-freeness:**
  - *TruePeakLimiter: detection is linked - a quiet channel gets the loud channel's gain*
  - *TruePeakLimiter: ceiling and release changes while limiting are click-free and never trip the safety clamp*
  - *TruePeakLimiter [adversarial]: ceiling automation while limiting never trips the safety clamp*
  - *TruePeakLimiter: sample-peak mode (true peak off) holds the sample ceiling with latency = lookahead*
- **RT safety and robustness:**
  - *TruePeakLimiter: reset, setParams and process do not allocate*
  - *TruePeakLimiter: silence, DC, full-scale noise, impulses and extreme settings stay finite and under the ceiling*
  - *TruePeakLimiter: NaN / Inf input never reaches the output and the limiter recovers*
  - *TruePeakLimiter: output is independent of the host block size (1, 7, 64, 512)*
  - *TruePeakLimiter: blocks narrower than the prepared channel count and long runs stay stable*
  - *TruePeakLimiter [adversarial]: a channel that leaves and rejoins never replays stale audio*
  - *TruePeakLimiter [adversarial]: reset() mid-limit forgets the gain; setParams() right after applies instantly*
  - *TruePeakLimiter [adversarial]: getGainReductionDb() reports the deepest gain of the block*
- **Detector primitives:** the three *TruePeakDetector* tests of §0.7 / §0.12 (`tests/test_primitives.cpp`) cover the shared interpolator.

### 10.9 Known limitations

- **True-peak scope.**
  - The sample peak is exact for any input.
  - True peak is within about 0.1 dB for content up to about 0.45 fs; measured at or below −0.023 dB re ceiling (§10.6).
  - Strong content between 0.45 fs and fs/2 is under-read, because no finite interpolator reaches fs/2. Raw digital white noise measured +0.90 to +1.35 dB above the ceiling, the most at 0.5 ms and 44.1 kHz (the header's "up to ~+1.2 dB" matches the 1.5 ms runs). Heavy maximizer clipping measured up to +0.17 dB on an ideal reconstruction (§11.3.5).
  - Mastered programme carries far less energy up there. On the 4× meter, every factory-preset render in `tests/test_factory_presets.cpp` stays below its ceiling (§1.4).
- **Look-ahead 0 ms is accepted** (API only; no profile uses it). The gain then switches instantly: the sample peak is exact, but the true peak is not guaranteed (about +1 dB on noise, implementer's measurement).
- **Release shape is fixed.** It is a one-pole in the linear gain domain; the run-gap and blend constants (25/25/50 ms) are not user parameters.
- **NaN counts as a clamp.** A NaN reaching the output is counted as a safety-clamp engagement (it is zeroed). `ProcessingChain` drops non-finite blocks before they get there.
- **The hold is fixed.** Kh = min(8, L/3) is a compile-time constant (`kTruePeakHold`), not a parameter. It is taken out of the attack ramp, so the ramp is Kh samples shorter (steeper) than the look-ahead alone would allow.

---

## 11. Loudness maximizer + soft clipper

Sources: [`core/include/flub/dsp/LoudnessMaximizer.h`](../core/include/flub/dsp/LoudnessMaximizer.h), [`core/src/dsp/LoudnessMaximizer.cpp`](../core/src/dsp/LoudnessMaximizer.cpp). Chain policy (glue arming and floor, AutoDrive, output trim): [`core/src/engine/ProcessingChain.cpp`](../core/src/engine/ProcessingChain.cpp), [`core/src/engine/MacroMap.cpp`](../core/src/engine/MacroMap.cpp) (`isArmed`).

### 11.1 Purpose

The maximizer is the last slot of every strip. It makes programme louder under a hard true-peak ceiling while keeping distortion measurable and bounded. It has three stages:
1. **Glue**: gentle 3-band 2:1 pre-compression. It balances the band levels so that one band, usually the bass, does not dominate the limiter.
2. **Soft clipper**: oversampled. It shaves sub-millisecond transients (snare or gunshot crack) cheaply, so the limiter only handles the longer peaks, with less audible pumping.
3. **True-peak limiter** at the ceiling (section 10).

It also publishes the telemetry the SafetyGovernor needs: limiter gain reduction (per block for the meter, `getGainReductionDb()`, and the deepest per fixed 10 ms window for the governor, `getWindowGainReductionDb()`: the limiter runs in pieces that end on that grid, so the governor input does not depend on the host block size), the clipper's measured THD+N (`getDistortionDb()`, §11.3.5) and the clip energy ratio, per block (`getClipEnergyRatioDb()`) and over the THD+N's analysis window (at least 25 ms, §14.5; `getWindowClipEnergyDb()`, which floors the clipper's share of the governor input).

### 11.2 Signal flow

```
 x ─► × drive (0…24 dB, 50 ms ramp in dB) ─► finiteOrZero
   ─► [glue]   (only while glue > 0; in the chain, only while glue is armed, §11.3.2)
               ThreeBandSplitter 120 Hz / 4 kHz (bands sum to an all-pass) ─► per band b:
                 linked |b| ─► 2-bucket hold ─► 5 / 80 ms follower ─► G_b = sqrt(T / env)  (env > T),  T = ceiling − 6 dB
               wet = Σ_b b · (1 + glue · (G_b − 1));        out = x + mix_glue · (wet − x)
   ─► [clip]   x̂ = Up(x) (2× or 4× half-band)  ─► c = clip(x̂) − x̂ ─► Down(c)
               y = x[n − L_os] + w_clip[n] · Down(c)[n]              (DELTA oversampling)
   ─► [limit]  TruePeakLimiter at the ceiling (look-ahead, 4× true peak)
 ─► (chain) output.gain trim −24 … 0 dB (20 ms ramp)
```

### 11.3 Algorithm & maths as implemented

#### 11.3.1 Drive

`x ← x · 10^(drive/20)`. The `exp` runs only while the drive ramps. A non-finite result, or a finite input pushed past the float range by the drive, becomes 0 at the door (`finiteOrZero`). It can then neither poison the glue splitter's recursive states nor smear through the oversampler FIRs. For finite input at 0 dB drive the path stays bit-exact.

#### 11.3.2 Glue

- **Split.** `ThreeBandSplitter` at 120 Hz and 4 kHz (§0.4). The low band passes the 4 kHz all-pass, so `low + mid + high = AP_4k · AP_120 · x`: flat magnitude, phase rotated.
- **Detection.** One *linked* level per band: `max over channels of |band|`. It then passes a two-bucket peak hold of `round(fs / (2 f_b))` samples, half a period of the lowest frequency the band carries:

  | Band | f_b | Hold at 48 kHz |
  |---|---|---|
  | low (< 120 Hz) | 30 Hz | 800 samples, 16.7 ms |
  | mid (120 Hz – 4 kHz) | 60 Hz | 400 samples, 8.3 ms |
  | high (> 4 kHz) | 2 kHz | 12 samples, 0.25 ms |

- **Follower and gain.**
  ```
  env ← held + c · (env − held),   c = attack (5 ms) if held > env else release (80 ms);   env < 1e−15 → 0
  T   = ceilingLin · 10^(−6/20) = ceilingLin · 0.50118723
  G   = env > T ? sqrt(T / env) : 1          i.e. out = T · (env / T)^½ : 2:1, hard knee, gain = −(L_env − T_dB)/2 dB
  bandGain = 1 + glue · (G − 1),   wet = Σ band · bandGain,   out = x + mix · (wet − x)
  ```
- **Telemetry.** `getGlueReductionDb()` is the block minimum over bands of `1 + mix · (bandGain − 1)`, in dB.
- **Anti-denormal and overflow guards.** An input offset alternating 1e−20 / 0 (DC plus Nyquist) keeps the splitter's SVF states normal in silence; they belong to the shared splitter and cannot be flushed from outside. If a band sum is non-finite (overflow of finite input), the splitter is reset.

**The chain's glue arming and floor.** `ProcessingChain` decides per block whether glue is *armed*:

```
glueArmed = base[max.glue] > 0  ||  MacroMap::isArmed(base, max.glue)
mp.glue   = glueArmed ? max(0.001, effective[max.glue]) : effective[max.glue]
```

`MacroMap::isArmed()` is true when a macro source that can raise the parameter in the current mode is above zero, even before it reaches its entry's start point. For glue that means Boost Intensity (Music, glue from 40 %) or the Music *Loudness* macro (glue from 30 %). No Gaming macro raises glue, so in Gaming glue is armed only by a preset or user value.

- **While armed**, the floor keeps the stage running. Switching glue fully off and on crossfades x against its own all-pass-shifted band sum. At equal weights the partial sum is `(x + AP_4k · AP_120 · x) / 2`. The all-pass product is within 5° of −1 at both crossover frequencies, so it nulls 120 Hz and 4 kHz by 27.6 dB (analytic, 48 kHz). The reviewer's time-domain measurement during a real 30 ms fade saw about 28 dB at 4 kHz and about 9 dB at 120 Hz, for a few ms. Without the floor this would happen every time Boost Intensity crossed the 40 % start of its glue contribution. At 0.001, `bandGain ≥ 1 − 0.001 = 0.999`: at most −0.0087 dB of band compression, measured −0.005 dB at 18 dB drive.
- **While disarmed** (the default: `max.glue` 0 and Boost and Loudness at 0), glue is 0 and the stage is out of the path. At 0 dB drive the maximizer then passes material that stays under the clip knee and the limiter threshold unchanged, only delayed.

Why not keep the floor on permanently: a phase rotator also changes the crest factor, and it raises it on flat-topped material. Sample-peak change through `AP_120 · AP_4k` (measured, 48 kHz, 8 seeds):
- a 100 Hz square wave: +3.4 to +3.7 dB;
- quasi-Gaussian white noise: +1.2 to +2.9 dB;
- pink-ish noise: −0.9 to +0.9 dB.

In the maximizer at 0 dB drive, ceiling −1 dBTP, synthetic "mastered" programme (saturated or clipped, peaks at −0.26 dBFS) was limited by **3.2–3.5 dB** (deepest block) with the floor, against 0.8–1.7 dB with `glue = 0`. Unclipped programme showed almost no difference (0.95 vs 0.82 dB). Test *Chain: with glue disarmed the maximizer passes hot flat-topped material untouched* checks the disarmed path on a −5 dBFS 100 Hz square: its true peak is −2.9 dBTP, but through the splitter it would read about +0.5 dBTP and be limited.

**Stage switching:**
- **On:** splitter and detectors start from rest and run unheard for 10 ms, then fade in over 30 ms.
- **Off:** a 30 ms fade-out, after which the stage stops and costs nothing. With `glue = 0` the stage is skipped entirely and the path is bit-exact.
- In the chain this happens when glue becomes armed or disarmed: when Boost Intensity or Loudness leaves zero or returns to it (in Music), and when a preset or the user sets `max.glue` above zero or back to it. Once armed, glue changes glide on the floor without switching the stage.

#### 11.3.3 Soft clipper: the curve

```
t  = 10^((ceilingDb + lerp(+6 dB, +0.3 dB, clipAmount)) / 20)       threshold (per base-rate sample)
ks = t · (1 − knee / 2)                                             knee start, ks ≥ t/2
clip(x) = x                                                         |x| ≤ ks
        = sign(x) · min(t, ks + (t − ks) · tanh((|x| − ks) / (t − ks)))    |x| > ks
knee = 0 → hard clip at t;   t ≤ 0 → 0
```

- **Properties:** odd, monotonic and C1 at ks: tanh has slope 1 at 0, so the join is continuous in value and slope. The curve approaches t asymptotically and **never exceeds t**. Because ks ≥ t/2, `t − ks` is exact (Sterbenz), `ks + (t − ks)·tanh(·)` cannot round above t, and a final `min(·, t)` is applied anyway. NaN falls through as NaN and is caught downstream.
- **At the defaults** (ceiling −1 dBTP, `max.clip` 0.5, `max.clipKnee` 0.5): headroom `lerp(6, 0.3, 0.5) = 3.15 dB`, so `t = +2.15 dBFS` (1.281) and `ks = 0.75 t = −0.35 dBFS` (0.961).
- `clipAmount = 1` puts t at −0.70 dBFS; `clipAmount → 0` would put it at +5.0 dBFS, but `clipAmount = 0` disables the clipper altogether.
- The threshold t always sits 0.3–6 dB *above* the ceiling. With a large knee the soft region can start below it: at knee 1, ks = t/2, 6 dB under t. The clipper shaves transients; the limiter then brings everything to the ceiling.

#### 11.3.4 Soft clipper: delta oversampling

The clipper runs at `osFactor · fs` through the half-band oversampler (§0.6). **Only the correction is band-limited**:

```
x̂[k] = Up(x)                         (factor 1: x̂ = x)
e    = Down( clip(x̂) − x̂ )           the deviation is the only thing that passes the half-band filters
y[n] = x[n − L_os] + w[n] · e[n]      x delayed exactly by the oversampler round trip; w = clip crossfade weight
```

- **Unclipped audio is bit-transparent.** Where nothing exceeds ks, `clip(x̂) − x̂ = 0` exactly, and y is the exactly delayed input.
- **The top octave does not droop.** If the programme itself passed the half-band filters, they would cut 20 kHz by 1.0 dB (High) to 2.3 dB (Low) at 44.1 kHz even when nothing clipped. Test *Transparency: the maximizer's oversampled clipper does not droop the top octave* holds 1, 15 and 19.5 kHz tones at −20 dBFS within **0.05 dB** for 2×/4×, Low/High, at 44.1 and 48 kHz.
- **Aliasing.** The harmonics the curve creates above the original Nyquist are attenuated by the downsampler's alias rejection (§0.6). Delta oversampling changes nothing here, because the correction is the only part that carries them.
- **Where the weight is applied.** The clip weight w is applied at base rate *after* the downsampler. With w = 0 the output is exactly the dry path whatever the filter tails hold, which keeps the path block-size independent.
- **Held controls.** The controls (t, knee, w) are computed per base-rate sample and held across its oversampled sub-samples. They move far too slowly for that to matter.

**Stage switching:**
- **On:** the oversampler is reset and runs unheard for `2 · L_os + 8` samples, then fades in over 20 ms.
- **Off:** a 20 ms fade-out; then the oversampler stops, but the dry delay keeps running so the latency is unchanged.

#### 11.3.5 THD telemetry

```
clipEnergyRatioDb = 10 log10( Σ (w · (x̂ − clip(x̂)))² / Σ x̂² )     over all oversampled samples and channels of the block
                  = −160 dB when nothing was clipped or the clipper is off
```

This is the energy the clipper removed (before band-limiting) relative to the energy that entered it: a measure of how hard the clipper works. It is published (`MeterBus::clipEnergyRatioDb`) and overstates distortion: part of the removed signal is in phase with the input, i.e. a gain change rather than distortion. The governor keeps it only as a floor under the clipper's measured THD+N, taken over the same 25 ms window (`getWindowClipEnergyDb()`, §14.5).

The clipper's share of the governor input is its **measured THD+N**, `getDistortionDb()`, floored at the clip energy ratio above taken over the same window (§14.5). The THD+N is taken in the same oversampled loop where `x̂` and the clipped `x̂` are aligned (`DistortionEstimator.h`, §14.5). With `r = w·(x̂ − clip(x̂))` the effective output is `x̂ − r`, and per channel over an analysis window of at least 25 ms (it closes at the first block boundary at or after 25 ms, §14.5)

```
distortionDb = 10 log10( Σ_ch (Σr² − (Σx̂r)² / Σx̂²) / Σ_ch Σ(x̂ − r)² )       = −160 dB when nothing was clipped or the clipper is off
```

i.e. the energy left after the least-squares gain, relative to the output energy. Only `Σx̂r` is new; the other two sums are the clip-energy sums. The reading covers harmonics up to `osFactor · fs/2`, before the downsampler removes those above the base-rate Nyquist. On white noise through four one-pole low-passes (−3 dB near 6 kHz), peak −6 dBFS before drive, 48 kHz, 4× High, defaults otherwise, it reads 0–3 dB below the clip energy ratio over the same windows (power averages over 3.5 s, THD+N / windowed clip energy / per-block clip energy: −81.9 / −81.9 / −81.9 dB at 6 dB drive, −38.3 / −38.0 / −38.1 dB at 9 dB, −21.8 / −20.7 / −20.7 dB at 12 dB, −10.7 / −7.9 / −7.9 dB at 18 dB, with 512-sample blocks; the same within 1 dB with 64-sample blocks). The SafetyGovernor's budget is **−30 dB** (≈ 3.2 % RMS) of THD+N, measured in the chain over the saturator and the clipper together (§14.5). The table below is the clip energy ratio, measured on noise band-limited to 0.4 fs, peak −6 dBFS (true peak −5.88 dBFS) before drive, 48 kHz, 4× High, 1.5 ms, defaults otherwise. "Disarmed" is `glue = 0`, the chain's default; "armed" is the 0.001 floor:

| Drive | Limiter GR (block min), disarmed / armed | Clip energy ratio (block max), disarmed / armed | Glue GR, armed | True peak re ceiling, disarmed / armed |
|---|---|---|---|---|
| 0 dB | 0.00 / 0.00 dB | −160 / −160 dB (no clipping) | 0.000 dB | −4.88 / −2.46 dB |
| 6 dB | −1.17 / −3.22 dB | −96.8 / −39.9 dB | −0.002 dB | −0.04 / −0.04 dB |
| 12 dB | −4.82 / −5.13 dB | −22.7 / −16.9 dB | −0.004 dB | −0.03 / −0.02 dB |
| 18 dB | −6.60 / −6.46 dB | −8.6 / −7.8 dB | −0.005 dB | +0.17 / +0.13 dB |

- At 0 dB drive the disarmed path returns the input's own true peak (−4.88 dB re ceiling); the armed splitter's all-pass raises it by 2.4 dB (§11.3.2).
- The same all-pass makes the armed stage clip and limit more at moderate drive: at 6 dB, 2.05 dB more limiting and clip energy 57 dB higher.
- At 12 and 18 dB drive this signal runs over the governor's −30 dB budget either way, in clip energy and in measured THD+N. In the chain the governor would scale the governed drive contributions back (section 14).
- The safety clamp never engaged.

#### 11.3.6 Limit and output trim

The internal `TruePeakLimiter` gets `{ceilingDb, releaseMs, autoRelease}`. `getGainReductionDb()` reports its block minimum. The limiter's look-ahead and true-peak detection are structural and passed through at `prepare()`.

After the maximizer the chain applies `output.gain`, a **trim of −24 … 0 dB** (20 ms linear ramp). It can only lower the level, so the strip's true-peak ceiling holds in every host, including the plug-in and the CLI, which have no master limiter.

#### 11.3.7 Block handling and channels

`process()` runs in segments of at most `maxBlockSize`. Prepared channels missing from a block are padded with silence from a pre-allocated `padBuffer`, so every stage (dry delay, oversampler, splitter, limiter) keeps running on all prepared channels and nothing stale is released when a wider block returns.

### 11.4 Parameters

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Loudness Maximizer | `max.on` | off/on | on | toggle | module bypass (`ModuleSlot`) |
| Drive | `max.drive` | 0 … 24 | 0 | dB | input gain into glue/clipper/limiter (+ governed macros; AutoDrive may reduce it) |
| Ceiling | `max.ceiling` | −12 … 0 | −1 | dBTP | limiter ceiling; clip threshold and glue threshold follow it |
| Clipper Share | `max.clip` | 0 … 1 | 0.5 | % | clip threshold headroom `lerp(6, 0.3 dB)`; 0 = clipper off |
| Clipper Softness | `max.clipKnee` | 0 … 1 | 0.5 | % | knee start `t (1 − knee/2)`; 0 = hard clip |
| Multiband Glue | `max.glue` | 0 … 1 | 0 | % | 3-band 2:1 amount; while armed (§11.3.2) the chain uses `max(0.001, value)` |
| Release | `max.release` | 5 … 1000 | 60 | ms | limiter release (section 10) |
| Auto Release | `max.autoRelease` | off/on | on | toggle | limiter program-dependent release |
| Loudness Target | `max.autoDrive` | off/on | off | toggle | AutoDrive loop (section 14) |
| Target Loudness | `max.target` | −24 … −6 | −14 | LUFS | AutoDrive target (gated output loudness) |
| Output Gain | `output.gain` | −24 … 0 | 0 | dB | post-maximizer trim (chain) |
| (clip oversampling) | `latency.profile` | 4× High / 4× High / 2× Low | Balanced: 4× High | — | structural; API accepts 1, 2, 4 (≤ 1 → 1, 2 → 2, ≥ 3 → 4) and High/Low |

Module sanitising: out-of-range values clamp; non-finite values keep the previous one.

### 11.5 Smoothing & click-freeness

| Change | Mechanism | Time |
|---|---|---|
| drive (dB), ceiling (dB), clipAmount, clipKnee, glue amount | linear ramps (per sample) | 50 ms |
| glue stage on/off | warm-up (unheard), then crossfade against the input | 10 ms + 30 ms in / 30 ms out |
| clipper on/off | warm-up `2 L_os + 8` samples, then crossfade against the aligned dry path | 20 ms |
| limiter ceiling | see §10.5 | 50 ms |
| module on/off | `ModuleSlot` crossfade against the latency-delayed dry path; reset + pre-roll L + 64 on re-activation | 20 ms |
| first parameters after `prepare()`/`reset()` | applied instantly (nothing to click against) | — |

The output is independent of the host block size (tested to 1e−5, including automation of every parameter and stage switch; bit-exact in practice).

### 11.6 Latency & CPU

**Latency = L_os + L + 20**: clip oversampler round trip + limiter look-ahead + detector delay. It is identical with the clipper on or off.

| Profile | Clip oversampling | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|---|
| Quality | 4× High (36) + 2 ms | 36 + 108 = 144 | 36 + 116 = **152** | 36 + 212 = 248 | 36 + 404 = 440 |
| Balanced | 4× High (36) + 1.5 ms | 36 + 86 = 122 | 36 + 92 = **128** | 36 + 164 = 200 | 36 + 308 = 344 |
| Low Latency | 2× Low (16) + 0.5 ms | 16 + 42 = 58 | 16 + 44 = **60** | 16 + 68 = 84 | 16 + 116 = 132 |

CPU (indicative, stereo, 12 dB drive). Re-measured after the glue-arming and limiter changes, in a pass where the `TruePeakMeter` reference read 76–78 ns (83–87 ns in §13.6), so compare rows within this table:

| Configuration | ns / stereo sample | % core |
|---|---|---|
| 4× High, clip 0.5, glue disarmed (Balanced / Quality default) | 367–386 | 1.8–1.9 % |
| 4× High, clip 0.5, glue floor 0.001 (armed) | 422–431 | 2.0–2.1 % |
| 4× High, clip 0.5, glue 0.5 | 407–421 | 2.0 % |
| 4× High, clip 0, glue floor | 232–242 | 1.1–1.2 % |
| clip 0, glue 0 (limiter only) | 161–163 | 0.8 % |
| 2× Low, clip 0.5, glue disarmed, 0.5 ms (Low Latency default) | 254–255 | 1.2 % |
| 2× Low, clip 0.5, glue floor, 0.5 ms | 321–333 | 1.5–1.6 % |

The 4× clipper costs about 205–225 ns, the glue splitter 35–80 ns while it runs, and the true-peak limiter about 160 ns.

### 11.7 Gaming vs Music usage

| | Music | Gaming |
|---|---|---|
| Boost Intensity | drive **+8 dB** (30–100 %, curve^1.2, governed); glue +0.3 (40–100 %); engages `max.on` from ≈ 26 % | drive **+6 dB** (30–100 %, curve^1.2, governed); engages `max.on` from ≈ 26 % |
| Mode macro | **Loudness**: engages `max.on`; drive **+10 dB** (0–100 %, curve^1.3, governed); glue +0.5 (30–100 %) | — |
| Max. effective drive at 100 % (governor scale 1) | 18 dB (+ base `max.drive`, clamped to 24) | 6 dB (+ base) |
| Latency profile | Balanced (4× High, 1.5 ms) or Quality | competitive presets: Low Latency (2× Low, 0.5 ms) |

The governed drive contributions are what the SafetyGovernor takes back when the average limiter GR goes below −6 dB or the measured THD+N of the saturator and the clipper above −30 dB. AutoDrive can additionally *reduce* the effective drive towards a loudness target, but never below 0 dB (section 14).

### 11.8 Tests that prove it

`tests/test_maximizer.cpp`:
- **Curve, latency, transparency:**
  - *LoudnessMaximizer: softClip is odd, continuous, monotonic, identity below the knee and never exceeds t*
  - *LoudnessMaximizer: latencySamples() = clip oversampler + limiter latency, constant with the clipper off*
  - *LoudnessMaximizer: drive 0 and a -20 dBFS signal pass unchanged, only delayed by latencySamples()*
- **Ceiling:**
  - *LoudnessMaximizer: with 18 dB drive the ceiling holds on noise and drum-like programme (sample and true peak)* (true peak ≤ +0.1 dB while clip energy ≤ −12 dB, ≤ +0.5 dB beyond; sample peak exact)
  - *LoudnessMaximizer: extreme and full-band input still holds the sample ceiling exactly*
  - *LoudnessMaximizer [adversarial]: 192 kHz and the latency profiles hold the ceiling with 18 dB drive*
- **Stages and telemetry:**
  - *LoudnessMaximizer: clipAmount 0 disables the clipper (telemetry -160 dB); clipping shows up in the telemetry*
  - *LoudnessMaximizer: the clipper shaves transients so the limiter reduces less*
  - *LoudnessMaximizer: glue > 0 reduces limiter gain reduction on a bass-heavy signal*
  - *LoudnessMaximizer [adversarial]: clip-energy telemetry equals the header formula*
  - *Distortion: the saturator's and the soft clipper's in-stage readings match a harmonic analysis of what the stages do within 0.1 dB* (`tests/test_distortion.cpp`: a 750 Hz sine at 0.6 peak; at 6, 9 and 12 dB drive the clipper's THD+N reading equals a Goertzel analysis of `softClip` on the same sine at 192 kHz; measured difference < 0.001 dB)
  - *LoudnessMaximizer [adversarial]: glue is 2:1 above ceiling - 6 dB and an all-pass (no gain) below it*
  - *LoudnessMaximizer: release and ceiling are passed to the limiter*
- **Click-freeness, RT safety, robustness:**
  - *LoudnessMaximizer: parameter changes and stage on/off switches are click-free*
  - *LoudnessMaximizer: reset, setParams and process do not allocate*
  - *LoudnessMaximizer: silence, DC, full-scale noise, impulses and extreme settings stay finite and under the ceiling*
  - *LoudnessMaximizer: NaN / Inf input is contained and the maximizer recovers*
  - *LoudnessMaximizer: output is independent of the host block size (1, 7, 64, 512)*
  - *LoudnessMaximizer [adversarial]: a channel that leaves and rejoins never replays stale audio*
  - *LoudnessMaximizer [adversarial]: a NaN in one channel does not glitch the other channel*
  - *LoudnessMaximizer [adversarial]: automation of every parameter and stage switch is block-size independent*

Elsewhere:
- *Transparency: the maximizer's oversampled clipper does not droop the top octave* (`tests/test_transparency.cpp`)
- *Chain: full Music boost on a hot programme never exceeds the ceiling* (`tests/test_engine.cpp`)
- *Chain: with glue disarmed the maximizer passes hot flat-topped material untouched* (`tests/test_engine.cpp`; ≤ 1e−6 against the delayed input, no gain reduction)
- *MacroMap: glue is armed only while a source that can raise it is off zero* (`tests/test_engine.cpp`)
- *SafetyGovernor: backs off under sustained over-limiting and recovers* (`tests/test_engine.cpp`)
- *Protection: the SafetyGovernor's clip-energy branch alone backs off at 15 %/s, holds inside its hysteresis and recovers at 3 %/s* and *Protection: governed Boost drive into heavy clipping trips the clip-energy budget; the governor scales only the governed contributions, never the base values, and releases when the signal calms* (`tests/test_protection_gaps.cpp`; details in §14.8)

### 11.9 Known limitations

- **True peak under very hard clipping.** A clip energy ratio above about −12 dB (the threshold the maximizer tests use), 18 dB beyond the governor's budget, creates intermodulation up to fs/2, which the limiter's detector under-reads. Measured up to +0.17 dB on an ideal reconstruction here (§11.3.5). `LoudnessMaximizer.h` reports that every factory preset at full macros stays below the ceiling on the 4× meter (worst −1.04 dBTP for a −1 dBTP ceiling); the instrumented renders of §1.4 agree. The sample peak is always exact.
- **While glue is armed the maximizer is not bit-transparent.** The floor keeps the all-pass `AP_4k · AP_120` in the path (flat magnitude, phase rotation), and the rotation raises the crest factor of flat-topped (already limited or clipped) material. The synthetic "mastered" signals of §11.3.2 got 1.8–2.7 dB *more* limiting at 0 dB drive than with glue 0. This now applies only while Boost Intensity or Loudness is off zero in Music, or a preset sets glue. Hot commercial masters are expected to behave similarly; this is not yet measured on real programme. The governor does not react, because it only acts on an average limiter GR below −6 dB.
- **Arming and disarming still comb briefly.** The stage switches on (30 ms fade-in after 10 ms warm-up) when glue becomes armed and fades out over 30 ms when it is disarmed. During each fade x is crossfaded against its all-pass (§11.3.2), so 120 Hz and 4 kHz dip for a few ms. This now happens when Boost Intensity or Loudness leaves or returns to zero, instead of at the glue start point. The same holds for any caller that passes `max.glue = 0` directly; it is inherent to "glue 0 skips the stage, bit-exact".
- **No automatic loudness compensation inside the module.** Loudness targeting is AutoDrive's job (reduce-only), and fair A/B is the loudness-matched bypass (section 14).
- **Clip controls are held per base-rate sample** across the oversampled sub-samples (inaudible at 50 ms ramps).

---

## 12. Spectral noise gate

Sources: [`core/include/flub/dsp/SpectralNoiseGate.h`](../core/include/flub/dsp/SpectralNoiseGate.h), [`core/src/dsp/SpectralNoiseGate.cpp`](../core/src/dsp/SpectralNoiseGate.cpp); FFT §0.8.

### 12.1 Purpose

The gate is light, adaptive noise reduction for hiss and hum on noisy captures, old recordings and batch restoration. It is an STFT gate with a per-bin noise floor learned by minimum tracking. Bins that do not rise clearly above their floor are attenuated by at most `reductionDb`, 12 dB by default: light by design. It has `fftSize` samples of latency, so the chain includes it **only in the Quality latency profile**. Voice-chat denoise is a separate, future neural module (**roadmap**, `docs/09-future-roadmap.md`).

### 12.2 Signal flow

```
 per channel (independent; up to 8):
 x ─► input FIFO (N) ───────────────────────────────────────────────────────────────┐
 y ◄─ overlap-add accumulator [0, H) ◄───────────────────────────────────────────────┤ (read before the new frame)
                                                                                    │
 every H = N/4 samples of stream time (hop clock shared by all channels):          ▼
   frame = FIFO · wa ─► FFT (N) ─► per bin k = 0 … N/2:
        p_k = |X_k|² · 2/N + 1e−20 ─► P_k (one-pole, τ_P) ─► N_k (bias-scaled minimum tracker)
        ρ_k = P_k / N_k ─► 3-bin mean ─► soft gate (6 dB transition) ─► per-bin attack/release (dB)
        ─► linear gain ─► 3-bin mean ─► X_k · G_k ─► IFFT ─► · ws ─► overlap-add
```

### 12.3 Algorithm & maths as implemented

**Framing.**
- `N = fftSize` is structural (`setFftSize()` before `prepare()`): a power of two from 256 to 4096, otherwise 512. The chain uses **1024** in the Quality profile.
- Hop `H = N/4` (75 % overlap), `N/2 + 1` bins, hop duration `h = H / fs`.
- At 48 kHz: N 512 → H 128 (2.67 ms), 257 bins; N 1024 → H 256 (5.33 ms), 513 bins.

**Windows and perfect reconstruction (COLA).**

```
wa[n] = sin(π n / N)              analysis  (periodic sqrt-Hann)
ws[n] = 0.5 · sin(π n / N)        synthesis
wa · ws = 0.5 sin²(π n / N);   Σ over the 4 frames covering any sample:
   sin²θ + sin²(θ + π/4) + sin²(θ + π/2) + sin²(θ + 3π/4) = (sin²θ + cos²θ) + (sin²(θ+π/4) + cos²(θ+π/4)) = 2
   → Σ wa·ws = 0.5 · 2 = 1
```

`Fft::forwardReal` is unscaled and `inverseReal` scales by 1/N, so unity gains reconstruct the input exactly: the implementer measured an error of about 1e−7. The test *reductionDb 0 is a perfect reconstruction delayed by fftSize* checks this. Each output sample is read from the accumulator *before* the incoming sample is analysed, so the delay is exactly N for every sample, independent of the hop phase.

**Per-bin power.**

```
p_k = |X_k|² · (2/N) + 1e−20          2/N = 1 / Σ wa²: white noise of variance σ² reads p_k = σ²
if p_k < 1e30:  P_k ← p_k + a_P (P_k − p_k),   a_P = exp(−h / τ_P),   τ_P = max(10 ms, 2 h)
```

- A bin whose raw power is NaN, Inf or ≥ 1e30 skips its update, so the statistics hold.
- `P_k` starts at 1e−20, which keeps every ratio and log finite.
- The two-hop minimum on τ_P takes over whenever the hop exceeds 5 ms. At 48 kHz that starts at N = 1024 (hop 5.33 ms, τ_P = 10.7 ms). It matters most for N ≥ 2048, where 10 ms would be less than one hop.

**Noise floor: bias-compensated minimum tracking.**

```
candidate c = B · P_k
first profile:  N_k = c
afterwards:     N_k = c                        if c < N_k        (follows downwards instantly)
                N_k = min(c, N_k · 10^(r·h/10))  otherwise        (rises at most r dB/s, r = floorRiseDbPerSec)
```

- **Bias B.** A minimum tracker on a fluctuating estimate settles on its *lower envelope*, not its mean. Uncompensated, the floor would sit 2 dB (short hops, fast rise) to 18 dB (long hops, slow rise) below the noise, and "threshold 6 dB above the floor" would open on plain noise.
  - `B` comes from a 2-D table, bilinear in (log2 h, log2 r).
  - Rows cover hops 0.3125 … 40 ms; columns cover r = 0.5 … 32 dB/s in octaves.
  - The values (0.85 … 9.14 dB) were generated by simulating this exact estimator on Gaussian white noise for 400 s.
  - At the defaults (r = 3 dB/s) B = **5.4 dB** for 512 @ 48 kHz and **7.1 dB** for 1024 @ 48 kHz.
  - With B, `N_k` estimates the *mean* noise power, so `thresholdDb` means "dB above the noise" for every fftSize, rate and rise rate. The reviewer's simulation found the mean floor error within ±0.05 dB of the true noise power across nine fs/N/r combinations.
- **When the floor may adapt:**
  - All four hops in the analysis window must be non-silent. A hop is *silent* if every sample has |x| < 1e−7 (−140 dBFS, digital silence); samples from before `reset()` count as silent.
  - Then `ceil(3 τ_P / h)` further valid frames must pass, so that P_k is within 5 % of settled: 12 frames at 512 @ 48 kHz and 6 at 1024. The first profile is therefore learned about 43 ms (512) or 53 ms (1024) after reset. Until then the gate is fully open.
  - Gaps between tracks do not erase the profile, and half-filled windows are never learned.
- **Freeze.** `freezeFloor` holds `N_k` completely, but only after an initial **learning period** of 1 s of adapting frames (375 frames at 512 @ 48 kHz). Before that the tracker adapts even with freeze on, because a profile frozen from a single ~10 ms snapshot would leave random bins permanently open.

**Gate gain.**

```
ρ_k      = clamp(P_k / N_k, 1e−20, 1e20)
ρ̄_k      = (ρ_{k−1} + ρ_k + ρ_{k+1}) / 3             mirrored at DC and Nyquist (Hermitian spectrum)
t        = clamp((10 log10 ρ̄_k − (thr − 3)) / 6, 0, 1)
target_k = −reduction · (1 − t)                        G_min at thr − 3 dB and below, unity at thr + 3 dB and above
g_k      ← target + c · (g_k − target),   c = exp(−h/attack) when opening (target > g), exp(−h/release) when closing
           snap to target within 1e−3 dB (an open bin is exactly 0 dB)
G_k      = 10^(g_k/20);   applied gain = (G_{k−1} + G_k + G_{k+1}) / 3 (mirrored);  all-unity stays exactly unity
```

**Musical-noise suppression.** Isolated bins of plain noise that randomly poke through the threshold and flicker on and off ("musical noise") are fought at five points:
1. The bias-compensated floor. The threshold refers to the *mean* noise power, so fewer noise bins cross it at all.
2. The 3-bin average of the *ratio*, not of the power. It cuts the variance of noise-only bins about 3×, and a stationary hum line cannot hold its neighbours open. At 1024 @ 48 kHz with 40 dB reduction it improves noise-only suppression from −25.7 to −31.8 dB (implementer).
3. A 6 dB soft transition instead of a binary decision.
4. Asymmetric per-bin ballistics. Bins open fast (5 ms) and close slowly (80 ms), which masks the random on/off flicker.
5. The 3-bin average of the *applied* gain. It keeps the gain curve smooth, which also keeps the equivalent filter kernel short and limits circular time-aliasing, and it spreads any isolated open bin.

**Measured noise-only attenuation.** White noise at −60 dBFS RMS, 48 kHz, defaults otherwise (threshold 6 dB, attack 5 ms, release 80 ms, rise 3 dB/s), averaged over 3–6 s:

| fftSize | reduction 12 dB | reduction 24 dB | reduction 40 dB (implementer) |
|---|---|---|---|
| 512 | −11.7 dB | −23.3 dB | −38.6 dB |
| 1024 (chain) | −11.2 dB | −21.8 dB | −31.8 dB |
| 4096 | −11.0 dB (implementer) | ≈ −22 dB (reviewer) | −28.0 dB |

The implementer measured that a −20 dBFS tone passes with less than 0.001 dB change, while noise outside 0.5–2 kHz drops by 11.4 dB. After a +20 dB noise step, the gate closes again after about `14 dB / r + 0.15 s`, independent of fftSize and rate.

### 12.4 Parameters

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Noise Gate | `gate.on` | off/on | **off** | toggle | module bypass; the slot exists only in the Quality profile |
| Gate Threshold | `gate.threshold` | 0 … 20 | 6 | dB | ρ̄ above the mean-noise floor at the centre of the 6 dB transition |
| Gate Reduction | `gate.reduction` | 0 … 40 | 12 | dB | maximum attenuation G_min; 0 = perfect reconstruction |
| Gate Attack | `gate.attack` | 1 … 50 | 5 | ms | per-bin opening time constant (hop rate) |
| Gate Release | `gate.release` | 10 … 500 | 80 | ms | per-bin closing time constant (hop rate) |
| Floor Adapt Rate | `gate.floorRise` | 0.5 … 20 | 3 | dB/s | maximum upward rate of the floor |
| Freeze Noise Profile | `gate.freeze` | off/on | off | toggle | hold the learned floor (after the 1 s learning period) |
| (FFT size) | `latency.profile` | 1024 in Quality | — | samples | structural; module accepts 256 … 4096 (default 512) |

Module sanitising: NaN/Inf keeps the previous value; values are clamped; an unchanged set returns early.

### 12.5 Smoothing & click-freeness

- `thresholdDb` and `reductionDb` glide per hop with a 20 ms one-pole (snap 1e−4), so moving either never steps every bin at once.
- Attack, release and rise are rates, so they take effect immediately without a discontinuity.
- Per-bin gain ballistics plus overlap-add make every gain change a smooth crossfade between overlapping frames.
- Module on/off: `ModuleSlot` crossfade (20 ms) against the dry path delayed by N. Re-activation resets the gate (the learned profile is forgotten) and pre-rolls N + 64 samples.
- Frames fall on absolute hop boundaries and all smoothing runs at hop rate, so the output is bit-identical for any host block size.

### 12.6 Latency & CPU

- **Latency = N**: 1024 samples in the chain (21.3 ms at 48 kHz, 23.2 ms at 44.1 kHz); 512 → 10.7 ms; 4096 → 85.3 ms at 48 kHz.
- CPU (indicative, stereo, 48 kHz): N 512 **245–259 ns** per stereo sample (1.2 %), N 1024 **262–279 ns** (1.3 %), N 4096 **282–316 ns** (1.4–1.5 %).
  - Per hop and channel it costs one forward and one inverse N-point transform, plus `N/2 + 1` each of `log10`, `exp` and division.
  - The reference FFT runs real transforms as full complex ones (§0.8), about 2× the cost of a real FFT. PFFFT/vDSP/IPP is roadmap.
- Memory, allocated in `prepare()`: per channel `2N + 3(N/2 + 1)` floats; shared `3N` floats, about `3(N/2 + 1)` scratch values and the FFT tables.

### 12.7 Gaming vs Music usage

- **Music / restoration.** The gate is available in the Quality profile (`gate.on`, default off; no macro touches it). Typical uses are hiss on old recordings and noisy captures, and the batch CLI.
- **Gaming.** It is not in the chain in Balanced or Low Latency, which the gaming presets use: 21 ms of latency is unacceptable there, and minimum tracking would learn steady game ambience as noise. Voice-chat denoise is a future neural module (roadmap).

### 12.8 Tests that prove it (`tests/test_noise_gate.cpp`)

- **Reconstruction and latency:**
  - *SpectralNoiseGate: reductionDb 0 is a perfect reconstruction delayed by fftSize*
  - *SpectralNoiseGate: fftSize is validated (power of two 256..4096, else 512)*
  - *SpectralNoiseGate: a low-level impulse appears exactly latencySamples() later*
- **Gating behaviour:**
  - *SpectralNoiseGate: noise-only input converges towards -reductionDb* (between −reduction − 0.5 dB and −0.85 · reduction)
  - *SpectralNoiseGate: -60 dBFS noise is reduced, a -20 dBFS tone passes untouched*
  - *SpectralNoiseGate: bins open fast (attack) and close slowly (release)*
- **Floor tracking:**
  - *SpectralNoiseGate: the floor climbs at floorRiseDbPerSec, independent of the hop length*
  - *SpectralNoiseGate: the floor follows a falling noise level at once*
  - *SpectralNoiseGate: freezeFloor holds the learned floor when the input level rises*
  - *SpectralNoiseGate: a frozen gate still learns its first profile after reset*
  - *SpectralNoiseGate: digital silence does not wipe the learned floor*
- **Parameters and channels:**
  - *SpectralNoiseGate: parameter changes are click-free*
  - *SpectralNoiseGate: setParams clamps to the documented ranges and ignores non-finite values*
  - *SpectralNoiseGate: stereo channels are processed independently*
  - *SpectralNoiseGate: a channel that drops out and returns restarts cleanly*
- **RT safety and robustness:**
  - *SpectralNoiseGate: reset, setters and process do not allocate*
  - *SpectralNoiseGate: silence, DC, full-scale noise, impulses and extreme settings stay finite*
  - *SpectralNoiseGate: output is identical for any host block size*
- **Review regressions:**
  - *SpectralNoiseGate: freezeFloor from the start freezes a settled profile, not a raw snapshot*
  - *SpectralNoiseGate: the gate converges quickly after reset (the floor starts high, not low)*
  - *SpectralNoiseGate: a NaN / Inf burst neither sticks in the output nor poisons the statistics*
  - *SpectralNoiseGate: reset() mid-stream is equivalent to a freshly prepared gate*
  - *SpectralNoiseGate: 8 channels at 192 kHz, block size 1 vs 4096, each equal to a mono gate*
  - *SpectralNoiseGate: a tone is preserved and noise reduced at the rate / size extremes*
  - *SpectralNoiseGate: per-block parameter automation stays finite and click-free on noise*

### 12.9 Known limitations

- **Minimum tracking treats any steady sound as noise.**
  - A tone or pad already playing when the gate starts, or lasting longer than about `(level − floor) / r` seconds, is learned and gated. `freezeFloor` is the tool against this.
  - With freeze on, whatever plays in the first second of valid material becomes the profile.
- **The profile is not persistent.** `reset()` forgets it; so does re-activation through `ModuleSlot` and `ProcessingChain::reset()`.
- **Pauses that are not digital silence are learned.** Dither-only passages become the floor, and recovery then takes `level difference / r` seconds.
- **Musical noise limits the depth at long hops.** With 40 dB reduction the implementer measured −31.8 dB (1024) and −28 dB (4096) on white noise.
- **Leakage near loud tones.** sqrt-Hann sidelobes fall at only 12 dB/oct, so a loud tone keeps nearby bins open: about ±1 kHz at 512 @ 48 kHz with 60 dB tone-to-noise.
- **Output peaks can exceed input peaks.** A time-varying spectral gain can raise output peaks above the input's: the implementer's worst case was 1.49× full scale. The downstream limiter bounds the result.
- **Changing `floorRiseDbPerSec` switches the bias B instantly.** Raising r can briefly drop the floor by up to about 4 dB, recovering in about 0.2 s. It is not a click.
- **The bias table assumes Gaussian noise.** Deterministic hum gets a floor B dB above it, so it is gated more firmly. Very narrowband or impulsive noise gets a floor that is somewhat off.

---

## 13. Metering: loudness, LRA, true peak, RMS and correlation

Sources: [`core/src/analysis/LoudnessMeter.cpp`](../core/src/analysis/LoudnessMeter.cpp), [`core/include/flub/analysis/LoudnessMeter.h`](../core/include/flub/analysis/LoudnessMeter.h), [`core/include/flub/analysis/ChannelWeights.h`](../core/include/flub/analysis/ChannelWeights.h), [`core/include/flub/analysis/PeakMeters.h`](../core/include/flub/analysis/PeakMeters.h), [`core/include/flub/analysis/LoudnessFollower.h`](../core/include/flub/analysis/LoudnessFollower.h); telemetry [`core/include/flub/engine/MeterBus.h`](../core/include/flub/engine/MeterBus.h).

### 13.1 Purpose

The meters provide standards-based measurement for the GUI, the loudness-matched A/B, the control loops and the CLI `analyze` command:

| Class | Measures | Standard |
|---|---|---|
| `LoudnessMeter` | momentary (400 ms), short-term (3 s), integrated (gated), LRA, max momentary / short-term | ITU-R BS.1770-4, EBU R128, EBU Tech 3341/3342 |
| `TruePeakMeter` | per-channel true peak, last block and hold since reset | BS.1770-4 Annex 2 (4× oversampling) |
| `LevelMeter` | per-channel sample peak (last block), RMS (300 ms), L/R correlation (300 ms) | IEC 60268-10-like ballistics |
| `LoudnessFollower` | cheap K-weighted running loudness (one-pole, default 3 s) | control loops only (section 14) |

All meters only *read* the block (`process()` takes it as const) and add no latency to the audio.

### 13.2 Signal flow in the chain

```
 input stage → fold to stereo ─► inLevel (LevelMeter), inLoudness (LoudnessFollower, 3 s)      → MeterBus in* fields
                                 dry copy ─► LoudnessMatch dry follower
 slots → output trim ─► SafetyGovernor / AutoDrive / LoudnessMatch wet (control loops)
                      ─► global-bypass crossfade
                      ─► outLevel (peak, RMS, correlation), outTruePeak (TruePeakMeter),
                         outLoudness (LoudnessMeter: M, S, I, LRA)                            → MeterBus out* fields
```

Every scalar is written once per block as a relaxed atomic and polled by the GUI at display rate. `MeterBus::resetLoudnessRequest` (GUI → audio) resets the integrated measurement and the true-peak hold.

### 13.3 Algorithm & maths as implemented

**K-weighting.** Two double-precision TDF-II biquads per channel. They are designed from the analog prototype through a bilinear transform prewarped at f0 (the libebur128/ffmpeg derivation), so every sample rate gets the same analog response:

```
stage 1 (pre-filter, high shelf): f0 = 1681.974450955533 Hz, G = 3.999843853973347 dB, Q = 0.7071752369554196
   K = tan(π f0 / fs),  Vh = 10^(G/20),  Vb = Vh^0.4996667741545416,  a0 = 1 + K/Q + K²
   b = [(Vh + Vb K/Q + K²)/a0,  2(K² − Vh)/a0,  (Vh − Vb K/Q + K²)/a0],   a = [1,  2(K² − 1)/a0,  (1 − K/Q + K²)/a0]
stage 2 (RLB high-pass): f0 = 38.13547087602444 Hz, Q = 0.5003270373238773
   d = 1 + K/Q + K²,   b = [1, −2, 1] (unnormalised, as in the BS.1770 table),   a = [1,  2(K² − 1)/d,  (1 − K/Q + K²)/d]
```

- **At 48 kHz** this reproduces the BS.1770 table: stage 1 b = [1.53512485958697, −2.69169618940638, 1.19839281085285], a = [1, −1.69065929318241, 0.73248077421585]; stage 2 a = [1, −1.99004745483398, 0.99007225036621].
- **Combined response** at 48 kHz (analytic): −13.28 dB at 20 Hz, −1.13 dB at 100 Hz, **+0.691 dB at 997 Hz** (cancelled by the −0.691 offset), +4.01 dB at 5 kHz, +4.04 dB at 10 kHz.
- **Rate dependence.** The unnormalised stage-2 numerator makes the passband slightly rate-dependent: 96 kHz and 192 kHz read 0.017 and 0.027 LU lower than 48 kHz, as libebur128 and ffmpeg do. The design rate is guarded: a non-finite rate becomes 48 kHz and the minimum is 8 kHz.
- **Anti-denormal offset.** A constant 1e−20 (−400 dBFS) is added at the input. The RLB double zero at DC removes it before squaring, but it keeps every recursive state normal during digital silence without FTZ.

**Channel weights** come from `bs1770ChannelWeight(channel, numChannels)` in `ChannelWeights.h`, shared by `LoudnessMeter` and `LoudnessFollower` so both read identically. BS.1770-4 gives 1.41 (+1.49 dB) to a loudspeaker between 60° and 120° azimuth, 1.0 to the others, and excludes the LFE. In the engine's WAVEFORMATEXTENSIBLE channel orders:

| Channels | Layout | Weights |
|---|---|---|
| 1–5 | no LFE / surround interpretation | all 1.0 |
| 6–7 | 5.1: FL FR FC LFE SL SR (+ extra) | ch 3 (LFE) 0; ch 4/5 (SL/SR, about 110°) 1.41; all others 1.0 |
| 8 | 7.1: FL FR FC LFE BL BR SL SR | ch 3 (LFE) 0; ch 4/5 (BL/BR, about 135–150°) **1.0**; ch 6/7 (SL/SR, about 90–110°) 1.41 |

The meter sets them in `prepare()` from the prepared channel count. A weight-0 channel (the LFE) is not filtered at all.

**Sub-blocks and windows.**

```
sub-block   = round(0.1 · fs) samples  (4410 / 4800 / 9600 / 19200 at 44.1 / 48 / 96 / 192 kHz — exact)
E_sub       = Σ_c G_c · Σ y_c²           (a channel whose sum is non-finite contributes 0 and has its filters reset)
ring        : 30 sub-blocks (3 s)
L(window)   = −0.691 + 10 log10( Σ E_sub / (count · subBlockLength) )     floored at −160
momentary   = newest 4 sub-blocks (400 ms);   short-term = all 30 (3 s)
```

- Readings update at exactly 10 Hz on the stream's 100 ms grid.
- Each reads −160 ("−∞") until its window has completely filled since `reset()`: 400 ms for momentary, 3 s for short-term.
- Host blocks are split at sub-block boundaries, per-channel sums extend in stream order and windows are summed oldest to newest. Every reading is therefore bit-identical for any host block size.

**Integrated loudness** (gated, BS.1770-4 / EBU R128):
- Every momentary window is a gating block: 400 ms, a new one every 100 ms, so 75 % overlap.
- Absolute gate: blocks at or below −70 LUFS are discarded.
- Relative gate: `E_thr = mean_E(abs-gated) · 10^(−10/10)`, i.e. 10 LU below the absolute-gated mean energy.
- `I = −0.691 + 10 log10(Σ E / Σ n)` over the blocks that pass both gates.
- Blocks are kept in a **two-level histogram** from −70 to +30 LUFS:
  - 1000 coarse bins of 0.1 LU, each split into ten fine bins of 0.01 LU;
  - every bin keeps the exact energy sum and an `int64` count, at both levels, plus running totals of all absolute-gated energy, so the relative threshold itself is exact;
  - the gate is decided on the single fine bin that straddles it, by that bin's exact mean energy;
  - coarse bins above it are included whole;
  - blocks louder than +30 LUFS (possible only with float overs) share the top bin with their true energy.
- The result is exact except for blocks within 0.01 LU of the gate, and none when those blocks are identical (steady material).
- Memory is O(1) for any programme length: 2 histograms × 11 000 bins × 16 B = 352 KB, allocated in `prepare()`.
- Each 100 ms update scans at most about 1000 coarse and 20 fine bins.

**Loudness range** (EBU Tech 3342):
- Every short-term value (3 s window, 100 ms hop) enters a second histogram of the same kind, with an absolute gate of −70 LUFS and a relative gate of −20 LU.
- With n gated values, the ranks are `round((n − 1) · 0.10)` and `round((n − 1) · 0.95)`, following the Tech 3342 reference indexing. Each rank resolves to its fine bin and is represented by that bin's exact mean loudness.
- `LRA = P95 − P10 ≥ 0`. It reads 0 until the first gated short-term value.
- libebur128 hops 1 s instead of 100 ms. The finer hop only gives the statistics more samples.

**Max values and resets.**
- `maxMomentary` and `maxShortTerm` are the largest readings since the measurement started.
- `reset()` clears everything.
- `resetIntegrated()` starts a new measurement (histograms, I, LRA, max values) while momentary and short-term keep running. Only windows that lie entirely after the reset join it; a partly accumulated sub-block is skipped.

**True peak** (`TruePeakMeter`): the 4× `TruePeakDetector` of §0.7 (161-tap Kaiser β 5 prototype, 40 taps/phase, `kDelay` = 20; phases flat within −0.02 / +0.04 dB up to 0.4535 fs). Per channel it keeps the maximum of the last block and a hold since `reset()`.
- The reading is the plain maximum of four 4× grid points, *without* the limiter's parabolic refinement. A short transient whose peak falls between grid points can therefore read up to `cos(π f / 4 fs)` low: −0.17 dB at 0.25 fs, −0.55 dB at 0.45 fs. Sustained tones eventually land on the grid (test: 15–20 kHz sines within ±0.05 dB).
- The chain publishes `outTruePeakDb` (max over L/R of the last block) and `outTruePeakMaxDb` (hold).

**Level and correlation** (`LevelMeter`):
- Per channel: the sample peak of the last block, and RMS as a `MeanSquareFollower` with τ = 300 ms (double state).
- For the first two channels, the correlation `r = <LR> / sqrt(<L²><R²>)`, with each mean a 300 ms one-pole. It reads 1 when `sqrt(<L²><R²>) ≤ 1e−12` (below about −120 dBFS), clamped to [−1, 1]: +1 is mono, 0 uncorrelated or wide, −1 out of phase.
- The spatializer computes its own output correlation for the mono safety (§7.3.7); the GUI's correlation meter is this one.

**LoudnessFollower** (control loops):
- K-weighting (the same stage designs), then the same `bs1770ChannelWeight()` weights, chosen per block from `min(block channels, prepared channels)`, then `MeanSquareFollower` with τ = 3 s by default.
- `getLufs() = −0.691 + 10 log10(ms)`, or −160 below 1e−20; `isActive()` is true above −60 LUFS.
- Guards: a 1e−20 input offset and a 1e−60 floor on the mean square keep states normal in silence, and a non-finite mean square resets the follower at the end of the block.

### 13.4 Parameters

The meters have no parameter keys. Related controls: the GUI's *reset loudness* action (`MeterBus::resetLoudnessRequest`), `bypass.matched` (section 14), and `autolevel.*` / `max.autoDrive` / `max.target` (section 14), which consume `LoudnessFollower`s.

### 13.5 Smoothing & ballistics

| Reading | Ballistics / window | Update |
|---|---|---|
| momentary / short-term LUFS | rectangular 400 ms / 3 s windows | 10 Hz, 100 ms grid |
| integrated, LRA | gated histograms since (re)start | 10 Hz |
| true peak | last-block max + hold since reset | per block |
| sample peak | last-block max | per block |
| RMS, correlation | exponential, τ = 300 ms | per sample, published per block |
| `LoudnessFollower` | exponential, τ = 3 s (100 ms for the gates' fast follower) | per sample |

### 13.6 Latency & CPU

- **Latency: 0.** Readings refer to audio already processed (test *latency is zero - readings refer to the audio already processed*). The true-peak detector's internal 20-sample interpolation delay only shifts *when* a peak is reported.
- CPU (indicative, stereo, 48 kHz): `LoudnessMeter` **8–9 ns** per stereo sample (two double biquads per channel plus bookkeeping per 100 ms), `TruePeakMeter` **83–87 ns** (3 × 40 multiply-adds per channel-sample), `LevelMeter` **8.5–9.3 ns**.

### 13.7 Gaming vs Music usage

The meters are identical in both modes. The GUI shows LUFS M/S/I and LRA, true peak, RMS and correlation for every strip. The chain meters the **stereo output** of the strip, after the virtualiser's binaural fold, so the 5.1/7.1 weights never apply there. They apply in the CLI's `analyze` of multichannel files and in the `LoudnessFollower`s that see the raw multichannel input (AutoLevel).

### 13.8 Tests that prove it

`tests/test_loudness_meter.cpp`:
- **K-weighting:**
  - *LoudnessMeter: K-weighting at 48 kHz equals the BS.1770 table*
  - *LoudnessMeter: K-weighting response shape and consistency across sample rates*
  - *LoudnessMeter (adversarial): K-weighting designs stay finite and stable for any rate*
- **Calibration and compliance:**
  - *LoudnessMeter: 0 dBFS 997 Hz sine in one channel reads -3.01 LUFS*
  - *LoudnessMeter: EBU Tech 3341 case 1 (-23 dBFS, 20 s) at 44.1 / 48 / 96 kHz* (± 0.1 LU)
  - *LoudnessMeter: EBU Tech 3341 case 2 (-33 dBFS, 20 s)*
  - *LoudnessMeter: EBU Tech 3341 case 3 (-36 / -23 / -36 dBFS, relative gate)*
  - *LoudnessMeter: EBU Tech 3341 case 4 (-72 / -36 / -23 / -36 / -72 dBFS, both gates)*
  - *LoudnessMeter: EBU Tech 3341 case 5 (-26 / -20 / -26 dBFS, 20 / 20.1 / 20 s)*
  - *LoudnessMeter: EBU Tech 3341 case 6 (5.1: L/R -28, C -24, Ls/Rs -30 dBFS)*
  - *LoudnessMeter: EBU Tech 3342 LRA cases* (± 0.1 LU)
  - *LoudnessMeter (adversarial): EBU Tech 3342 LRA cases at 44.1 and 96 kHz*
  - *LoudnessMeter: 192 kHz still meets Tech 3341 case 1*
  - *LoudnessMeter: 5.1 / 7.1 channel weighting (LFE excluded, surrounds +1.5 dB)* (5.1 SL/SR and 7.1 SL/SR +1.49 dB, 7.1 BL/BR 0 dB, LFE silent, no interpretation below 6 channels)
- **Windows, gating and resets:**
  - *LoudnessMeter: window timing - readings appear exactly when their window is full*
  - *LoudnessMeter: momentary window is a rectangular 400 ms window on a 100 ms grid*
  - *LoudnessMeter: absolute gate - silence does not pull integrated loudness down*
  - *LoudnessMeter: max momentary / short-term hold the loudest window*
  - *LoudnessMeter: resetIntegrated starts a new measurement, momentary keeps running*
  - *LoudnessMeter: reset clears everything*
  - *LoudnessMeter (adversarial): histogram gating matches a brute-force BS.1770 / Tech 3342 reference* (I within 0.002 LU, LRA within 0.02 LU, at 44.1–192 kHz and several block sizes)
  - *LoudnessMeter (adversarial): a noise passage sitting right on the relative gate*
  - *LoudnessMeter (adversarial): reset() / resetIntegrated() exactly on and off the 100 ms grid*
- **Contract and robustness:**
  - *LoudnessMeter: process() only reads the block*
  - *LoudnessMeter: process, reset and getters do not allocate*
  - *LoudnessMeter: robustness - silence, DC, full-scale noise, impulses at all rates*
  - *LoudnessMeter: NaN / Inf input cannot poison the meter*
  - *LoudnessMeter (adversarial): NaN in one channel only drops that channel's sub-block*
  - *LoudnessMeter: extreme configuration values are clamped safely*
  - *LoudnessMeter (adversarial): blocks with fewer / more channels than prepared*
  - *LoudnessMeter: readings are independent of the host block size*
  - *LoudnessMeter: latency is zero - readings refer to the audio already processed*
- **Follower:**
  - *LoudnessFollower: converges to the programme loudness and gates silence*
  - *LoudnessFollower (adversarial): recovers from NaN / Inf input*

True peak: the three *TruePeakDetector* tests of §0.7 / §0.12 (`tests/test_primitives.cpp`).

### 13.9 Known limitations

- **Layouts are fixed by channel count.** The weights assume the Windows WAVEFORMATEXTENSIBLE orders above. A 6- to 8-channel file in another order (for example film order L C R) is weighted as if it were 5.1/7.1 in that order, and a 7-channel stream is read as 5.1 plus an unweighted extra channel.
- **Update rate.** Readings update at 10 Hz, so a value can lag the newest audio by up to 100 ms (this meets the EBU minimum). At non-standard rates where `0.1 · fs` is not an integer (e.g. 11 025 Hz) the windows are off by a fraction of a sample.
- **Gate comparisons are strict (`>`).** Tech 3342 writes `≥`; this matters only for values exactly on a threshold.
- **The true-peak meter uses the plain 4× grid** (no refinement), which BS.1770-4 permits. Short transients near fs/2 can read up to about 0.5 dB low; the limiter uses the refined detector.
- **Channel-count mismatch.** `LoudnessFollower` picks its surround weights from the *block's* channel count, the meter from the *prepared* count. They differ only when a block carries fewer channels than prepared.

---

## 14. Macros, Music/Gaming modes & protection loops

Sources: [`core/src/engine/MacroMap.cpp`](../core/src/engine/MacroMap.cpp), [`core/src/engine/Parameters.cpp`](../core/src/engine/Parameters.cpp), [`core/src/engine/ProcessingChain.cpp`](../core/src/engine/ProcessingChain.cpp), [`core/src/engine/Protection.cpp`](../core/src/engine/Protection.cpp) with [`core/include/flub/engine/Protection.h`](../core/include/flub/engine/Protection.h), [`core/src/engine/MixEngine.cpp`](../core/src/engine/MixEngine.cpp), [`core/include/flub/engine/DeviceProfiles.h`](../core/include/flub/engine/DeviceProfiles.h).

### 14.1 Base values, effective values and the macro formula

Presets and the GUI only write **base** values, through `ParameterStore::set()`. It ignores a NaN (the stored value is kept), clamps ±Inf and every other value to the parameter's range, and bumps the version counter; `Info::clamp()` on its own maps NaN to the default (§0.1). Every audio block, `ProcessingChain::applyParameters()` computes **effective** values from the base snapshot and pushes them into the modules:

```
effective[p] = clamp_p( base[p] + Σ_e  amount_e · c_e(v_e) · g_e )

  v_e      = value of the entry's source: Boost Intensity or Macro 1..5 (0..1); entries with v_e ≤ 0 are skipped
  c_e(v)   = smoothstep(start_e, end_e, v) ^ exponent_e       (smoothstep(t) = 3t² − 2t³)
  g_e      = SafetyGovernor scale (0.3..1) if the entry is "governed", else 1
  clamp_p  = the parameter's own [min, max] from the layout
```

- **Toggles.** Module enables can be macro targets. A toggle reads *on* at ≥ 0.5.
  - An entry `{source, XOn, +1, 0, 0.02}` (`kEngage = 0.02`) switches its module on as soon as the macro reaches 1 % (smoothstep(0, 0.02, 0.01) = 0.5). For example, *Warmth* engages Saturation.
  - Contributions clamp at 1, so a macro can switch a module on but never off.
  - The Boost entries for the maximizer (25–27 %) and, in Gaming, the compressor (5–7 %) switch on at about 26 % and 6 %.
- **Governed entries.** Entries whose effect is "more loudness or drive" are **governed**. The governor can take them back without touching tonal or spatial entries.
- **Governor timing.** The scale applied in a block is the one computed after the previous block.
- **Staggered windows.** Staggered `start/end` windows make one slider behave "intelligently": different processes arrive in a musically sensible order. The first third mostly adds clarity, width and detail, the middle adds bass and harmonics, and loudness ramps in last.
- **Arming.** `MacroMap::isArmed(base, paramId)` is true when a source that can raise the parameter in the current mode (an entry with a positive amount) is above zero, even before the entry's start point. The chain uses it for the glue floor (§11.3.2).
- **Mode and format overrides.** After `MacroMap::apply`, `applyParameters()` writes the chain's policies into the same effective array: in Gaming, crossfeed 0 and, for a compressor only macros switched on with the ratio at its default, ratio 1:1 (§14.4); under the binaural lock, width 1, space 0 and crossfeed 0; below 42 kHz, air 0 (§5.3.4). The modules receive exactly these values.
- **GUI read-back.** `ProcessingChain::effectiveValue(paramId)` returns the last effective value to the GUI for "ghost" markers (0 for an invalid id). It is published at the end of `applyParameters()`, after the overrides above, so the markers show what is applied rather than what the store and macros asked for (`prepare()` publishes the plain post-macro values, which the first processed block replaces). The audio thread publishes the whole array once per block into relaxed atomics, so the read is race-free from any thread. `ProcessingChain::modeBandFrequency(mode, band)` gives the GUI the centre or corner frequency of the dynamic-EQ mode bands (`kFirstModeBand = 4`, `kNumModeBands = 4`; 0 for any other band).

### 14.2 Boost Intensity staging

Every entry from `kMusicTable` / `kGamingTable` (amount at 100 %, active window, curve exponent; \* = governed):

| Target | Music | Gaming |
|---|---|---|
| Presence (`clarity.presence`) | +0.35 (0–50 %) | +0.30 (0–50 %) |
| Air (`clarity.air`) | +0.30 (10–60 %) | — |
| Transient attack (`clarity.attack`) | +2 dB (10–60 %) | +2 dB (20–70 %) |
| Width (`spatial.width`) | +0.20 (0–50 %) | — |
| Positional focus (`spatial.focus`) | — | +0.30 (0–60 %) |
| Upward detail (`comp.upMax`) | — | +5 dB (10–70 %) |
| Bass boost (`bass.boost`) | +5 dB\* (20–80 %) | +3 dB\* (30–90 %) |
| Harmonic bass (`bass.harmonics`) | +0.30\* (35–90 %) | — |
| Maximizer drive (`max.drive`) | +8 dB\* (30–100 %, curve^1.2) | +6 dB\* (30–100 %, curve^1.2) |
| Glue (`max.glue`) | +0.30 (40–100 %) | — |
| Saturation drive (`sat.drive`) | +4 dB\* (60–100 %) | — |
| Engages | Dynamic EQ (from > 1 %, so the Music de-boom band can track Boost), Maximizer (≈ 26 %) | Compressor (≈ 6 %, for upward compression; 1:1 downward unless a ratio is set, §14.4), Maximizer (≈ 26 %) |

`dyneq.on`, `max.on`, `bass.on`, `clarity.on` and `spatial.on` default to on, so their engage entries matter only if a preset or the user switched the module off. `comp.on` and `sat.on` default to off.

### 14.3 Music mode macros (Macro 1–5)

| Macro | Targets (amount at 100 %, active window; \* governed) | Internal dynamic-EQ companion (mode bands, set by `configureModeBands()`) |
|---|---|---|
| **Punch** | Clarity on; transient attack +6 dB (0–100 %); bass tighten +0.5 (20–100 %) | — |
| **Width** | Stereo on; width +0.6 (0–100 %); space +0.35 (40–100 %) | — |
| **Clarity** | Clarity on; presence +0.8 (0–100 %); air +0.7 (20–100 %); de-mud +0.5 (0–70 %); Dynamic EQ on | Band 4: **de-harsh** bell 3.5 kHz, Q 1.2, *cut above* −22 dBFS, 3:1, range 3 dB × Clarity, 2 / 80 ms. Band 5: **air** high shelf 12 kHz, Q 0.7, *boost below* −45 dBFS, 2:1, range 3 dB × Clarity, 10 / 200 ms |
| **Loudness** | Maximizer on; drive +10 dB\* (0–100 %, curve^1.3); glue +0.5 (30–100 %) | — |
| **Warmth** | Saturation on; saturation drive +9 dB\* (0–100 %); harmonic bass +0.2\* (40–100 %); bass boost +2 dB\* (30–100 %); Bass on | — |

- **Always-on Music companion.** Band 6 is a **de-boom** bell at 120 Hz, Q 1.0, *cut above* −14 dBFS, 2.5:1, with range 4 dB × Boost Intensity (10 / 150 ms). When the bass is boosted, boomy passages are held in check dynamically. Band 7 is unused in Music: a 1 kHz bell with range 0, so idle.
- **Floor.** All Music mode bands use a −80 dBFS noise floor.
- **Idle bands.** A mode band is enabled only while its range exceeds 0.01 dB, so it costs nothing while its macro is at 0.

Maximum effective values with Boost and all five macros at 100 % (governor scale 1, default base values):

| Parameter | Value | Parameter | Value |
|---|---|---|---|
| presence | 1.0 (1.15 clamped) | bass boost | +7 dB |
| air | 1.0 (0 below 42 kHz, §5) | harmonic bass | 0.5 |
| transient attack | +8 dB | maximizer drive | +18 dB |
| width | 1.8 | glue | 0.8 |
| space | 0.35 | saturation drive | +13 dB |

### 14.4 Gaming mode macros (Macro 1–5)

| Macro | Targets (\* governed) | Internal dynamic-EQ companion |
|---|---|---|
| **Footsteps** | Dynamic EQ on; Compressor on (upward only unless a ratio is set, see the policies below); upward max gain +3 dB (30–100 %) | Band 4: **footstep detail** bell 3.2 kHz, Q 0.9, *boost below* −42 dBFS, 3:1, range 7 dB × Footsteps (3 / 120 ms), floor −75 dBFS. Band 5: **footstep body** bell 260 Hz, Q 1.2, *boost below* −45 dBFS, 2.5:1, range 3 dB × Footsteps (5 / 150 ms), floor −75 dBFS. Band 6: **explosion anti-masking** low shelf 90 Hz, Q 0.7, *cut above* −22 dBFS, 3:1, range 6 dB × Footsteps (10 / 250 ms), floor −80 dBFS |
| **Positional** | Stereo on; positional focus +0.9 (0–100 %); width +0.25 (30–100 %, widens only above `spatial.lowCut`, default 180 Hz). Raises the ILD of partially panned sources; a hard-panned source keeps its infinite ILD (the focus's polarity guard, §7.3.2; §7.9) | — |
| **Impact** (explosions, gunshots) | Bass on; bass boost +6 dB\* (0–100 %); harmonic bass +0.25\* (40–100 %); Clarity on; transient attack +4 dB (20–100 %) | — |
| **Detail** (environment) | Compressor on (upward only unless a ratio is set); upward max gain +8 dB (0–100 %); Clarity on; air +0.4 (20–100 %) | — |
| **Voice & Score** | Clarity on; presence +0.7 (0–100 %); de-mud +0.4 (20–100 %); Dynamic EQ on | Band 7: **voice / score** bell 2 kHz, Q 0.7, *boost below* −36 dBFS, 2:1, range 4 dB × Voice (5 / 150 ms), floor −70 dBFS |

Maximum effective values with Boost and all macros at 100 %:
- presence 1.0, positional focus 1.0 (1.2 clamped), transient attack +6 dB, air 0.4, de-mud 0.4;
- width 1.25 (forced to 1 under the binaural lock);
- upward max gain +16 dB. With the default upward curve the lift itself tops out at +9 dB (§9.3);
- bass boost +9 dB, harmonic bass 0.25, maximizer drive +6 dB.

**Why this footstep design works.**
- Footsteps are quiet, transient and broadband, with their identifying energy around 2–5 kHz (the scuff and tick) and 150–400 Hz (the heel).
- *Boost-below* dynamic bands are upward compressors confined to those regions. They lift quiet events and leave loud ones (gunfire and explosions, which exceed the threshold) untouched, so the mix does not get louder overall.
- The noise-floor taper keeps silence and ambience beds from being pumped up. Band 4's lift fades out over the 10 dB above −75 dBFS (§3.3).
- The anti-masking shelf only engages on *very* loud low-frequency events and recovers with a 250 ms release, so the steps after an explosion are not buried.
- All detection is stereo-linked (dynamic EQ, compressor, limiter), so none of this moves a source's apparent direction.

**Gaming policies enforced by the chain** (independent of preset values; written into the effective values, so `effectiveValue()` reports them, §14.1):
- **Crossfeed forced to 0.** It blurs interaural differences, the main lateral localisation cue.
- **A macro-engaged compressor is upward-only.** When Boost Intensity, Footsteps or Detail switched the compressor on (base `comp.on` off) and `comp.ratio` is still at its default 2.5, the effective ratio is 1:1: quiet cues are lifted, gunshots and explosions keep their dynamics. Presets that set a ratio (e.g. 1.5:1 glue) and a compressor the user switched on keep theirs (§9.7).
- **Binaural lock** (both modes). When the virtualiser rendered 5.1/7.1 to binaural (`inputChannels > 2` and `virt.on`), the chain sets width 1, space 0 and crossfeed 0. Positional focus, a mild ILD emphasis around 1–6 kHz on the side channel, stays available.
- **Low Latency presets.** Presets for competitive play (`gaming-competitive-fps`, `gaming-battle-royale`, `gaming-tournament-clean`) choose the *Low Latency* profile: 0.5 ms look-aheads and 2× short (Low-quality) oversampling. That is **100 samples = 2.08 ms** of algorithmic latency at 48 kHz, 96 samples = 2.18 ms at 44.1 kHz (section 15).

### 14.5 Protection loops

```
 in ─► input gain ─► AutoLevel ─► fold ─► [slots … maximizer (limiter @ max.ceiling)] ─► output trim ─┬─► global bypass ─► meters ─► strip out
        ▲ (+1/−4 dB/s)                                     │ GR, THD+N (sat + clipper)              │
        │                                                  ▼                                        ├─► AutoDrive ──► max.drive reduction (next block)
        └── measured on the input (open loop)       SafetyGovernor ──► scale on governed macro     └─► LoudnessMatch wet ─► dry-path gain in bypass
                                                    amounts (next block)
 global bypass reference: fold ─► dry delay (chain latency − limiter latency) ─► × match gain ─► TruePeakLimiter @ max.ceiling ─► crossfade
                          (the limiter runs only while bypass is engaged)
 desktop app: Σ strips (padded to equal latency) ─► master TruePeakLimiter (−1 dBTP, or the device cap) ─► device
```

| Loop | Measures | Acts on | Law (as implemented) |
|---|---|---|---|
| **SafetyGovernor** (THD / over-processing) | Maximizer limiter gain reduction (the deepest per fixed 10 ms window, counted from the maximizer's reset: `getWindowGainReductionDb()`; a block that closes several windows reports their mean) and the distortion of the saturator and the maximizer's soft clipper, per block: the **measured THD+N** of each (below), the clipper's floored at its clip energy ratio, power-summed. Averaged with a one-pole `a = exp(−Δt / 3 s)`: GR in dB, distortion in the power domain (so bursts are not under-weighted). A fully bypassed maximizer feeds 0 dB GR and no clipper distortion; a fully bypassed saturator feeds no THD+N. The bass harmonics generator and the air exciter are measured too, but are deliberately **not** an input (below). | Scale on all *governed* macro amounts (never the base values) | **Over budget** (avg GR < −6 dB **or** avg distortion > −30 dB): scale −0.15 per second (−15 %/s), floor 0.3; 1 → 0.3 takes 4.7 s. **Comfortably under** (avg GR > −4.5 dB **and** distortion < −31.5 dB: 1.5 dB hysteresis on both): +0.03 per second (+3 %/s) up to 1; 0.3 → 1 takes 23 s. In between it holds. |
| **AutoLevel** (LUFS input levelling) | *Gated* K-weighted loudness of the input (all input channels with the BS.1770-4 channel weights of §13.3, before its own gain, so open-loop and unconditionally stable). A 3 s one-pole advances only while programme is present: block RMS > −70 dBFS, 100 ms follower > −50 LUFS and within 20 LU of the slow value. It counts as active only while the slow value is > −60 LUFS. **Relative-gate release:** the slow value only moves while the gate is open, so a programme more than 20 LU quieter than the last one would be held out for good; after 3 s of programme kept out by the relative criterion alone (silence and the absolute gates pause that count, an open gate clears it) the slow value restarts and acquires the new level. **Cold start:** after a reset or restart the one-pole reads 10 log10(1 − e^(−t/3 s)) dB low after t seconds of open gate (−8 dB at 0.5 s, −3 dB at 2 s); `GatedLoudness::getLufs()` adds that back, so the reading is the exponentially weighted mean over the open-gate time so far and is unbiased from the first block (the uncorrected reading made AutoLevel lift a source already at its target by up to 2.4 dB, and move one 6 dB over it the wrong way first). | Input gain before the fold and the slots | Gain = target − measured, clamped ±12 dB, slew +1 dB/s up and −4 dB/s down, adapted only while the gate is open. Pauses, track gaps and fade-outs never pump the gain up. Switched off, it returns to 0 dB at 4 dB/s. Applied as a per-block linear ramp. Target `autolevel.target` −30 … −10 LUFS (default −18). |
| **AutoDrive** (maximizer loudness target) | Gated loudness (same gate) of the strip *output*, after the output trim | Maximizer drive | Closed loop with a 0.5 LU dead band. It integrates the error at min(2, 0.5 · \|error\|) dB/s. The chain passes the requested drive (effective `max.drive`) to `AutoDrive::update()`, and the reduction stays in [−requested drive, 0] dB (the requested drive clamped to 0 … 24 dB); the chain applies `drive = max(0, max.drive(effective) + reduction)`. It can only **reduce** the requested drive, never below 0 dB, so it never makes anything louder than the user or macros asked for (and cannot make a programme that is already above target at 0 dB drive quieter). Because the reduction stops where the drive reaches 0 dB, it holds no reduction beyond that point: the drive starts coming back as soon as the output falls more than 0.5 LU below the target, and a lowered drive clamps the reduction at once. (It used to run on towards −24 dB with nothing audible changing, and had to climb back from there before any drive returned.) Switched off, the reduction returns to 0 at 4 dB/s. Target `max.target` −24 … −6 LUFS (default −14). |
| **LoudnessMatch** (fair A/B) | Gated loudness of the dry reference (post-fold, pre-slots) vs the processed output | Gain on the dry path in global bypass | Gain = wet − dry, clamped ±12 dB, slew 3 dB/s, updated only while both followers are active. A positive match is additionally capped once per block at `max(0, max.ceiling − held dry peak)`; the dry sample peak is held with an instant attack and a ~2 s one-pole release, applied per block. The gain itself ramps over 50 ms, so a new, louder dry peak can still meet a gain chosen for the quieter past; the bypass-reference limiter (next row) catches that. |
| **Bypass-reference limiter** (`dryLimiter`, every host) | The matched dry reference, 4× interpolated peaks | Reference gain in global bypass | A `TruePeakLimiter` at `max.ceiling`, 80 ms auto release, true-peak detection. Look-ahead 1 ms (48 samples at 48 kHz), capped at chain latency − 20; with the 20-sample detector that is 68 samples at 48 kHz, taken out of the dry-path delay (`dryDelay` = chain latency − limiter latency), so the reference stays aligned and no latency is added. If a chain's latency were too short for the detector plus 8 samples it would fall back to sample-peak detection with the whole chain latency as look-ahead; the shipped profiles never need that. It runs only while bypass is engaged (the crossfade is above 0 or moving) and is `reset()` whenever it starts, so it never resumes from stale history. Started cold, it outputs silence for its latency (1 ms + 20 samples: 68 samples = 1.42 ms at 48 kHz, 64 = 1.45 ms at 44.1 kHz) at the very start of the 30 ms crossfade, where the dry weight is still below 5 % at 44.1 kHz and above (at narrowband rates the 20 detector samples are a larger share: 28 samples = 3.5 ms at 8 kHz, about 12 % of the crossfade). |
| **True-peak ceiling** (strip) | 4× interpolated peaks with parabolic refinement | Limiter gain | Look-ahead sliding-minimum + box-filter envelope reaches the required gain Kh samples before the peak arrives and holds it Kh samples after (Kh = 8 at the profile look-aheads); a final safety clamp counts any engagement, published per block as `MeterBus::safetyClipCount` (section 10). |
| **Output trim** | — | Strip level after the maximizer | `output.gain` −24 … 0 dB (20 ms ramp). A trim cannot raise the level, so the strip ceiling also holds in the plug-in and the CLI, which have no master limiter. |
| **Master limiter** (desktop app, `MixEngine`) | Sum of all strips, each padded to the largest strip latency | Master gain | −1 dBTP, 1 ms look-ahead (68 samples at 48 kHz including the detector); 0.5 ms (44 samples) when every strip runs Low Latency. 50 ms auto release. Engages only when several strips overlap hot. Its safety-clamp count is `MixEngine::getMasterSafetyClipCount()`. |
| **Device ceiling cap** (desktop app) | Output device transport and profile (`device::adviceFor()`) | Master limiter ceiling | Default −1 dBTP; **Bluetooth A2DP −2 dBTP** (lossy codecs overshoot); **Bluetooth hands-free −3 dBTP**. A profile's own `ceilingDbTp` can lower it further; no shipped profile sets one yet. Strip ceilings are untouched: the cap only ever lowers the output. |
| **Input sanitation** | Non-finite samples in the input block (Σ x·0 is non-finite) | Signal path | The block is output as silence and the signal-path state (modules, delays, meters) is reset; the control loops above keep theirs (§0.11). |
| **Parameter sanitation** | Every `ParameterStore::set()` | Base values | NaN is ignored (the previous value stays), ±Inf and out-of-range values clamp to the parameter's range, so no automation, host or script value can put a NaN gain into the audio path. |

**Measured distortion (THD+N).** Every nonlinear stage the governor watches measures its own distortion over analysis windows of at least 25 ms (next points) with `flub/dsp/DistortionEstimator.h`, *inside the stage*, around its curve, where the curve's input and output are time-aligned and at the same (oversampled) rate, so no latency or oversampler delay has to be compensated:

```
g        = <x, y> / <x, x>                         least-squares linear gain of the window (per channel)
residual = Σ_ch <y − g x, y − g x> = Σ_ch ( <d, d> − <x, d>² / <x, x> ),   d = y − x (the curve's deviation)
THD+N    = 10 log10( residual / Σ_ch <y, y> )       dB re the output energy; −160 dB = none
```

- **Stages.** The maximizer's soft clipper (at 2× / 4×, the clip weight included; §11.3.5) and the Saturator (at its oversampling factor, with depth, mix and output gain folded in; §6.3). Accumulating `x` and `d` instead of `x` and `y` avoids the cancellation of `<y, y> − <x, y>²/<x, x>`: a linear stage reads the rounding floor (tested below −90 dB; exactly −160 dB when `d = 0`). The cost is three double multiply-adds per oversampled sample and channel; the chain CPU change is within run-to-run noise with the saturator off (575–597 against 586–605 ns per stereo sample, Balanced, defaults, 48 kHz, 512-sample blocks, measured under load) and about +2 % with it on (1044–1070 against 982–1037 ns, Boost 60 % + Warmth 50 %).
- **Window, not block.** The sums accumulate in a `DistortionWindow` until at least 25 ms of base-rate samples (1200 at 48 kHz) have been counted; the window closes at the first block (or segment) boundary at or after that point, so it spans 25 ms up to 25 ms + one block, and exactly one block when blocks are longer than 25 ms (at 48 kHz: 2048 samples in 1024-sample blocks, 4096 in 4096-sample blocks). The stage publishes its reading when a window closes and holds it until the next; on programme the window length, and with it the energy weighting and the publish rate, therefore follows the block size. Over a block much shorter than a bass period, the fundamental and its harmonics are nearly collinear, so a per-block `g` absorbs most of the harmonics: per block, a 55 Hz sine through 12 dB of tape drive read 8 dB low in 64-sample blocks and 14 dB low in 32-sample blocks. With the window the reading is the same from 32- to 4096-sample blocks (measured within 0.05 dB at 30–2000 Hz; test *Distortion: the readings do not depend on the host block size: a 55 Hz tone through the saturator and the clipper reads the same in 32- and 4096-sample blocks*). The governor and the meter see the reading up to one window (at least 25 ms, longer with large host blocks: 85 ms in 4096-sample blocks at 48 kHz) late, which is small against their 3 s and 300 ms time constants.
- **Accuracy.** On a sine through tanh and through the Tube curve, the estimate equals a Goertzel harmonic analysis (DC + harmonics 2 … N/2 over the total) to < 0.001 dB from −60 to −10 dB; in the stages it matches a harmonic analysis of what the stage does to < 0.01 dB (`tests/test_distortion.cpp`). For a static curve on a sine, `g x` is exactly the fundamental, so the residual is exactly DC + harmonics; on programme it also contains intermodulation. The estimate includes harmonics above the base-rate Nyquist that the downsampler removes (they would otherwise alias).
- **Combination and smoothing** (`DistortionMonitor`, `Protection.h`). The latest window readings of the stages in series are power-summed every block (the saturator's residual passes the clipper at the same ratio to the signal; the two residuals are taken as uncorrelated; `DistortionMonitor::combineDb`). The governor gets the same power sum with the clipper's reading floored at its clip energy ratio over the same window (next point) and averages it over ~3 s in the power domain as above. For display, a power-domain one-pole with **τ = 300 ms** smooths it into `MeterBus::distortionDb` (the clip energy ratio stays in `MeterBus::clipEnergyRatioDb`).
- **Budget: −30 dB (≈ 3.2 % RMS of the output), with the clip energy as a floor.** This keeps the number of the former clip-energy budget. The clip-energy proxy counts the part of the removed signal that is in phase with the input, which is a gain change, not distortion: on a steady tone the clipper's THD+N reads 3–4 dB below it (soft clipper at its default knee, near the budget), on band-limited noise 0–3 dB (§11.3.5). On its own, the clipper's THD+N would therefore let the governor back off later on steady clipping than the proxy did, so the clipper's share of the governor input is `max(THD+N, clip energy ratio)`, both over the same analysis window: the governor then acts at least as early as on the proxy, one window later at most (test *Distortion: through the chain, the clipper's share of the governor input is floored at its clip energy ratio, so clipping backs the scale off at least as far as the proxy alone did*), and saturation is added on top. On programme the windowed reading is also higher than the former per-block proxy: a window weights its loud (clipped) stretches by their energy, while the proxy's per-block ratios entered the 3 s average with equal weight. Measured on the drum-like programme of the protection tests (kick, noise hats, 55 Hz bass line, pad; peaks −6.7 dBFS, 12 s, 48 kHz, 512-sample blocks), minimum governor scale with this input / with the former per-block proxy / with the windowed THD+N alone: at each preset's own settings, Club Loud 0.39 / 0.92 / 0.65 (its THD+N averages −32.3 dB, with peaks above the budget) and every other factory preset 1.00 / 1.00 / 1.00. At Boost Intensity 100 % the scale is at or below the proxy's on every factory preset, for example Earbuds 0.41 / 0.51 / 0.46, Horror Detail 0.36 / 0.44 / 0.50, Punchy Pop 0.31 / 0.41 / 0.46, Late Night 0.72 / 0.77 / 0.96, and Lo-Fi Chill and Warm Vinyl 0.30 / 1.00 / 0.30 (the proxy could not see their saturation; THD+N −26.5 and −23.1 dB). The THD+N alone would have left three of them above the proxy's scale. Saturation is now counted too: the saturator's THD at 6 dB drive on a −6 dBFS tone is already about −26 dB (§6.3), so saturation that uses up the budget by itself leaves Boost and Warmth no room to add more.
- **Governed contributions only.** Decision 4 of `00-understanding-and-plan.md` and §14.1 define the governor as scaling the governed macro *contributions*; the base values (`max.drive`, `sat.drive` from a preset or the user) are the user's explicit choice and are never touched. The governor therefore reacts to all measured distortion, including what base values cause, but only takes back what the macros add. The measurement itself is published (`MeterBus::distortionDb`), so the distortion that base settings cause can be shown; the app does not display it yet.
- **The intentional harmonic generators: measured, kept apart, not a governor input.** The bass engine's harmonics (§4.3.4) and the clarity air exciter (§5.3.4) add a generated signal `a` to a dry path `x` on purpose. Each measures, over the same analysis windows, the share of what it generates in its output (`BassEngine::getDistortionDb()`, `ClarityEnhancer::getDistortionDb()`; −160 dB while harmonics / air are off), where the harmonics are added and the paths are aligned. Their band-split filters matter here: the shaper's input `b` is a filtered copy of the input, and below full scale an odd Chebyshev term has a part proportional to `b` (the exciter on its band's skirt; the bass shaper on programme whose envelope is not one tone's own amplitude), which the post filters phase-shift against `x`. A scalar fit against `x` counts that linear branch as distortion (a 9.6 kHz tone through the exciter read 12 dB high). The stages therefore fit the output against **two** linear references, the dry path `x` and the linear branch `p` (`b` through the same post filters and mix as the shaper output), and count only what neither explains (`flub/dsp/ParallelDistortion.h`):
  ```
  residual = min over (α, β) of ‖ x + a − α x − β p ‖²      (per channel, Gram–Schmidt on the six sums xx, xp, pp, xa, pa, aa)
  reading  = 10 log10( Σ_ch residual / Σ_ch ‖x + a‖² )     dB re the stage output at that point
  ```
  For a steady tone this equals a harmonic analysis of the stage output to < 0.001 dB (40 / 80 Hz through the bass harmonics at every character, with and without replacing the fundamental; 4–6 kHz and the 9.6 kHz skirt tone through the exciter, its air shelf taken out, since the reading is taken before that linear shelf), reads −160 dB with harmonics / air off whatever the other stages do, and is the same from 32- to 4096-sample blocks (within 0.13 dB for a 55 Hz tone, 0.002 dB for the exciter). On programme it also contains intermodulation, as the THD+N does.
  **What they read, and the policy.** Measured like the protection rows above (drum-like programme, factory presets, 12 s, 48 kHz, 512-sample blocks; power averages after 2 s): the bass harmonics read **−3.5 dB** (Laptop Speakers, whose Small Speaker Mode replaces the fundamental) to −42 dB on the 7 presets that use them at their own settings, −2.9 to −25 dB on 16 presets at Boost 50 % and −1.7 to −17.5 dB at Boost 100 %; the air exciter reads −45 to −88 dB on 22 presets. Harmonics are the point of these stages, so power-summing them into the −30 dB budget makes the governor fight the macros that ask for them. Presets whose minimum governor scale falls below 1 (in brackets: how many of those reach the 0.3 floor), for each way of feeding the harmonic readings to the governor:

  | Governor input | Own settings | Boost 0 % | Boost 50 % | Boost 100 % |
  |---|---|---|---|---|
  | saturator + clipper only (**chosen**; unchanged) | 0 | 0 | 1 (0): Lo-Fi Chill 0.83 | 5 (1): Lo-Fi Chill 0.30, Club Loud and Warm Vinyl 0.46, Punchy Pop 0.94, Bluetooth Headphones 0.99 |
  | + harmonics at full weight | 6 (6) | 6 (6) | 16 (16) | 16 (16) |
  | + harmonics at −6 dB | 6 (6) | 6 (6) | 15 (7) | 16 (16) |
  | + harmonics at −10 dB | 6 (6) | 6 (6) | 7 (6) | 16 (16) |

  (24 factory presets. The six pinned at their own settings are Earbuds, Laptop Speakers, Cinematic Adventure, Racing, Bass Head and Club Loud.) No weight helps: keeping Laptop Speakers at its own settings off the budget needs a weight below −26.5 dB, at which every other preset stays under the budget even at Boost 100 % (Bass Head: −5.1 − 26.5 = −31.6 dB), so the weight would only ever act on Small Speaker Mode, the most deliberate use of the harmonics; and the governor could not take back air at all (`clarity.air` has no governed macro contribution), only Boost's other governed amounts. So both readings are **excluded from the governor input and from the THD+N meter** (`MeterBus::distortionDb` would otherwise show −2 to −25 dB whenever harmonic bass is on). `DistortionMonitor::updateHarmonics()` power-sums and smooths them (τ = 300 ms) separately, so they can be shown as what they are; `MeterBus` does not carry them yet. Test *Distortion: through the chain, the bass harmonics and the air exciter are not a governor input: harmonics far over the -30 dB budget leave the scale at exactly 1 and the THD+N meter at -160 dB, where a governor fed them would back off to its floor* asserts the policy end to end (the laptop setting on programme reads −2.1 dB). Cost: the chain with harmonic bass 0.6 (+6 dB boost) and air 0.6 went from 639–656 to 664–682 ns per stereo sample (+4 %, Balanced, 48 kHz, 512-sample blocks, same session); the second reference costs the bass engine three and the exciter two SVF sections per sample (and channel), and six double multiply-adds per sample and channel.

### 14.6 Bypass, A/B and latency profiles

- **Per-module bypass** (`ModuleSlot`):
  - A 20 ms linear, equal-gain crossfade (`y = d + mix · (w − d)`) against a dry path delayed by the module's latency.
  - A bypassed module is not processed at all.
  - A re-enabled module is reset and pre-rolled for latency + 64 samples while still fully dry, so look-ahead lines and filters are primed before anything is heard.
  - The chain latency is identical whether a module is on or off.
- **Global bypass** (`bypass`): a 30 ms crossfade to a dry reference taken after the input stage and fold and delayed to the full chain latency. The delay is split: a plain `DelayLine` of chain latency − 68 samples (at 48 kHz) and, while bypass is engaged, the bypass-reference `TruePeakLimiter` (68 samples), so the reference is true-peak limited at `max.ceiling` without adding latency (§14.5). With *loudness-matched bypass* (`bypass.matched`, default on), the reference is also level-matched (§14.5), so comparisons are about tone and dynamics, not loudness. On programmes with high peaks the ceiling cap can leave the matched reference below the processed loudness: in a re-measurement on synthetic pink noise with 55 Hz kicks (about −22.8 LUFS, peaks −5.2 to −6.3 dBFS, Loudness macro 100 %, 48 kHz), the settled reference read 1.2–1.5 LU below the processed output, and every render in all three profiles peaked at −1.05 dBTP / −1.05 dBFS.
- **A/B:** two complete parameter banks in the `ParameterStore`. The switch is one atomic, and all continuous parameters glide inside the modules.
- **Latency profiles** (`latency.profile`, structural):
  - Changing the profile sets `needsReprepare()`. The host re-prepares the chain off the audio thread, which is a deliberate, short dropout.
  - Totals at 48 kHz are below; section 15 has the other rates.

| Profile | Gate (STFT) | Saturator OS | Compressor LA | Clipper OS | Limiter LA | Total @ 48 kHz |
|---|---|---|---|---|---|---|
| Quality | in chain (1024) | 2× High (32) | 3 ms (144) | 4× High (36) | 2 ms + 20 (116) | 1352 smp ≈ 28.2 ms |
| Balanced (default) | — | 2× Low (16) | 1 ms (48) | 4× High (36) | 1.5 ms + 20 (92) | 192 smp = 4.0 ms |
| Low Latency | — | 2× Low (16) | 0.5 ms (24) | 2× Low (16) | 0.5 ms + 20 (44) | 100 smp ≈ 2.1 ms |

### 14.7 Parameters (global, macros, protection)

| Name | Key | Range | Default | Unit | What it does |
|---|---|---|---|---|---|
| Input Gain | `input.gain` | −24 … +24 | 0 | dB | before AutoLevel, 20 ms ramp |
| Output Gain | `output.gain` | −24 … 0 | 0 | dB | trim after the maximizer, 20 ms ramp |
| Mode | `mode` | Music, Gaming | Music | choice | selects the macro table, mode bands and gaming policies |
| Boost Intensity | `boost` | 0 … 1 | 0 | % | staged macro source (§14.2) |
| Macro 1 … 5 | `macro.1` … `macro.5` | 0 … 1 | 0 | % | Music: Punch, Width, Clarity, Loudness, Warmth. Gaming: Footsteps, Positional, Impact, Detail, Voice & Score |
| Auto Level | `autolevel.on` | off/on | off | toggle | input LUFS levelling |
| Auto Level Target | `autolevel.target` | −30 … −10 | −18 | LUFS | AutoLevel target |
| Loudness-Matched Bypass | `bypass.matched` | off/on | on | toggle | level-matched dry reference in global bypass |
| Bypass All | `bypass` | off/on | off | toggle | global bypass (30 ms crossfade) |
| Latency Profile | `latency.profile` | Quality, Balanced, Low Latency | Balanced | choice | structural (re-prepare) |
| Loudness Target | `max.autoDrive` | off/on | off | toggle | AutoDrive |
| Target Loudness | `max.target` | −24 … −6 | −14 | LUFS | AutoDrive target |

### 14.8 Tests that prove it

`tests/test_engine.cpp`:
- **Parameters and macros:**
  - *Parameters: layout is complete, keys unique, defaults in range*
  - *ParameterStore: clamping, banks, snapshot, version*
  - *ParameterStore: NaN is ignored, infinities clamp, and the chain stays finite* (every parameter set to NaN, then a programme through the chain: all output finite)
  - *MacroMap: zero macros leave base values untouched*
  - *MacroMap: Boost Intensity is staged and governed*
  - *MacroMap: glue is armed only while a source that can raise it is off zero*
  - *MacroMap: every mode has five named macros that engage modules*
- **Protection loops:**
  - *SafetyGovernor: backs off under sustained over-limiting and recovers* (6 s at −12 dB GR: scale < 0.6 and ≥ 0.3; then it rises)
  - *AutoLevel: brings a quiet source towards the target, slew limited, frozen in silence*
  - *Chain: AutoDrive pulls a hot drive down to the loudness target at <= 2 dB/s and settles near, not above, it* (+16 dB drive, 7 LU over a −18 LUFS target: reduction in [−24, 0] dB, never faster than 2 dB/s, exactly 2 dB/s while far over, within 0.75 LU of the target by 10 s and never back above it, settles at −19 … −17.25 LUFS)
  - *Chain: AutoDrive never raises the drive: below the target it is inert, and it stops at 0 dB drive (the input itself)*
  - *Chain: AutoDrive's reduction stops at the requested drive, so it recovers at once* (+6 dB drive and an unreachable −24 LUFS target: after 12 s the reduction is −6 dB within 1e−3, not lower; lowering `max.drive` to 2 dB lifts it to ≥ −2 dB at once)
  - *Chain: matched bypass reproduces the processed loudness within 0.5 LU; unmatched bypass is the input itself* (+10 dB drive on a programme peaking at −25 dBFS, so neither the cap nor the reference limiter acts: reference within 0.5 LU of the processed loudness and the input × 10 ± 0.5 dB; unmatched = delayed input within 1e−6)
- **Bypass and latency:**
  - *ModuleSlot: bypassed slot is a pure latency-compensated delay and toggling is click-free*
  - *Chain: latency per profile and constant under module bypass* (Balanced < 5 ms, Low Latency < 2.5 ms)
  - *Chain: everything bypassed = input delayed by the chain latency (bit-transparent path)* (≤ 1e−6)
- **Ceiling and chain behaviour:**
  - *Chain: full Music boost on a hot programme never exceeds the ceiling* (both modes, all macros 100 %: true peak ≤ −1 dBTP + 0.15 dB, sample peak ≤ −1 dBFS, `MeterBus::safetyClipCount` 0)
  - *Chain: with glue disarmed the maximizer passes hot flat-topped material untouched*
  - *Chain: process() is allocation-free in every mode and profile*
  - *Chain: 7.1 input is virtualised (or downmixed) to stereo; extra channels cleared*
  - *Chain: toggling the virtualiser on a 7.1 strip crossfades (no step in the output)*
  - *Chain: loudness-matched global bypass tracks the processed loudness without exceeding the ceiling*
  - *Chain: matched bypass never overshoots the ceiling when a louder dry peak arrives* (Loudness macro 100 %, kicks rising from −26 to 0 dBFS over 10 s, bypass engaged at 4 s, all three latency profiles: from 100 ms after engaging, true peak ≤ −1 dBTP + 0.15 dB and sample peak ≤ −1 dBFS + 1e−6 on both channels)
  - *MixEngine: strips are summed, padded to equal latency and master-limited*
  - *Chain: a NaN/Inf input block is dropped and the chain recovers*
  - *Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz)*

`tests/test_protection_gaps.cpp`:
- **SafetyGovernor, distortion branch** (these two tests predate the measured THD+N; the unit test's "clip energy" values now enter the governor's distortion input, and in the chain test the budget is tripped by the heavily clipping maximizer, whose share of that input is its clip energy ratio or its measured THD+N, whichever is higher, while the test mirrors the published clip energy ratio):
  - *Protection: the SafetyGovernor's clip-energy branch alone backs off at 15 %/s, holds inside its hysteresis and recovers at 3 %/s* (GR input 0 dB throughout: −20 dB clip energy lowers the scale by 0.15 ± 0.002 per second to exactly 0.3; clean input releases it at 0.06 ± 0.002 per 2 s once the average is under −31.5 dB; −31 dB holds it unchanged for 20 s; one 10 ms block at −3 dB trips it, one at −15 dB does not)
  - *Protection: governed Boost drive into heavy clipping trips the clip-energy budget; the governor scales only the governed contributions, never the base values, and releases when the signal calms* (base drive 10 dB + Boost 100 %, clipper share 1, 8 s of hot programme: the mirrored 3 s GR average never goes below −4.5 dB while the clip-energy average peaks ≥ 6 dB over its budget (−11 dB measured), the scale reaches ≤ 0.35; every block's effective drive, bass boost, harmonics and saturation drive equal base + amount × the previous block's scale within 1e−4, presence, width and glue never move, and the store is unchanged; 20 s of quiet programme bring the scale back up by ≥ 0.1 (0.30 → 0.55 measured))
- **Measured THD+N** (`tests/test_distortion.cpp`):
  - *Distortion: on a sine through tanh (and the asymmetric tube curve) the estimator matches a Goertzel harmonic analysis within 0.02 dB from -60 to -10 dB*
  - *Distortion: a linear gain stage reads below -90 dB; the identity, silence and linear stage settings read -160 dB* (gains 0.25 … 3.98 and −1 on noise; the saturator at 0 dB drive with +6 dB make-up and the maximizer below its knee read exactly −160 dB; 12 dB of tape drive on a −80 dBFS sine reads below −90 dB)
  - *Distortion: the saturator's and the soft clipper's in-stage readings match a harmonic analysis of what the stages do within 0.1 dB*
  - *Distortion: the readings do not depend on the host block size: a 55 Hz tone through the saturator and the clipper reads the same in 32- and 4096-sample blocks* (0.3 sine, tape 12 dB / maximizer 12 dB drive, blocks 4096, 1024, 128, 64 and 32: within 0.2 dB of the 4096-sample reading, measured < 0.05 dB; with a per-block estimate the saturator read 4, 9 and 14 dB low at 128, 64 and 32)
  - *Distortion: the monitor power-sums the stages and smooths the meter in the power domain with tau = 300 ms* (a step reaches 1 − 1/e of its power after 0.3 s, ±0.02 dB, for 480- and 64-sample blocks)
  - *Distortion: the SafetyGovernor backs off when the measured THD+N of a real stage exceeds the -30 dB budget, and not when it stays under* (the saturator's own reading on a sine, 3–6 dB over the budget: scale at the 0.3 floor after 10 s; 3–6 dB under it: scale 1 throughout)
  - *Distortion: through the chain, base saturation alone trips the governor on measured THD+N (the clip-energy proxy stays silent) and only the governed Warmth contributions are scaled* (maximizer off, so GR 0 dB and clip energy −160 dB every block; base tape drive 12 dB + Warmth 100 % on hot programme: `MeterBus::distortionDb` peaks ≥ 6 dB over the budget, the scale ends ≤ 0.35, every block's effective `sat.drive` = base + 9 dB × the previous block's scale within 1e−4, the store is unchanged; Warmth 30 % on quiet programme: measurable THD+N ≥ 3 dB under the budget, scale exactly 1)
  - *Distortion: through the chain, the clipper's share of the governor input is floored at its clip energy ratio, so clipping backs the scale off at least as far as the proxy alone did* (the maximizer alone with the clipper at its maximum share on a 750 Hz sine at 0.94, 8 s, Boost 0 so the scale changes no audio: the clip energy averages ≥ 1 dB over the budget (−27.8 dB) and the THD+N ≥ 0.5 dB under it (−31.1 dB); in every block the chain's scale is at most that of a SafetyGovernor fed the published GR and per-block clip energy, plus 0.004 for the one-window delay, and both reach ≤ 0.6; with the floor removed the chain's scale stays at 1, 0.7 above the mirror's)
  - *Distortion: measuring in the saturator and the clipper, the monitor and the governor update are allocation-free* (`tests/test_rtsan.cpp` checks the `FLUB_NONBLOCKING` annotations of the estimator, `DistortionMonitor::update` / `reset` and `SafetyGovernor::update` / `reset`)
- **LoudnessMatch as a unit:**
  - *LoudnessMatch: the dry gain converges to the measured wet - dry loudness difference, slewed at exactly 3 dB/s without overshoot* (+6, −9, 0 and +11.5 dB: ≤ 0.03 dB per 10 ms block, 3 dB after 1 s, no reversal above 1e−4 dB, settled within 0.05 dB)
  - *LoudnessMatch: the gain is bounded to +-12 dB, holds while either side is silent, and reset() returns it to 0 dB*
- **Gated loudness, dropped blocks, bypassed meters and the governor's GR input:**
  - *GatedLoudness: programme more than 20 LU below the last one is held out for 3 s, then the slow measure restarts, so AutoLevel converges on it and AutoDrive releases its reduction* (−8 LUFS for 20 s, then −30 LUFS: AutoLevel at −10 dB and AutoDrive at its −12 dB floor hold through 2.5 s of the quiet programme and 5 s of silence, then AutoLevel climbs monotonically at ≤ 1 dB/s to +12 ± 0.2 dB and AutoDrive returns to 0 dB within 30 s; without the release they stayed at −9.4 and −12 dB)
  - *GatedLoudness: the slow measure is corrected for its cold start, so AutoLevel leaves a source at its target alone and never moves the wrong way* (from the first open block a fresh measure reads within 0.1 dB of the settled one; AutoLevel from reset stays within ±0.15 dB on a source at its target and never rises on one 6 dB over it, reaching −6 ± 0.15 dB by 1.5 s; uncorrected: +2.48 dB, +0.71 dB and −1.97 dB)
  - *Chain: a dropped NaN/Inf block resets the signal path but keeps the converged governor, AutoLevel and AutoDrive state* (Boost 100 % into heavy clipping with AutoLevel on: across the dropped block the scale (< 0.6) and the AutoLevel gain (< −3 dB) move by no more than their own slew; a full reset moved them by 0.7 and 8.5 dB)
  - *Chain: a maximizer, compressor, bass engine or dynamic EQ switched off mid-stream publishes no gain reduction and no clip energy (its last readings are not held)*
  - *Protection: the governor's limiter-GR input is taken per fixed 10 ms window, so the same programme and drive govern the same at 64- and 4096-sample blocks* (maximizer alone at 14 dB drive, no clipper: minimum scale in 64- and 4096-sample blocks within 0.05 of the 512-sample one, which is between 0.5 and 0.9; per-block minimum: 1.0 / 0.68 / 0.3)
- **A/B and bypass clicks:**
  - *A/B: bank switches and global bypass toggles (matched and unmatched) are click-free on a sine* (1 kHz; bank B = +9 dB EQ at 1 kHz, presence, saturation on, width 1.5, +3 dB drive; around every switch and bypass toggle the largest sample-to-sample step stays within 1.1 × the steady signal's own, measured 1.000–1.002)
- **Music Width and Clarity macros:**
  - *Macros: Music Width engages Stereo and raises width 1 -> 1.6 and space 0 -> 0.35 (from 40 %) monotonically, ungoverned, clamped at 2*
  - *Macros: Music Clarity engages Clarity and Dynamic EQ and raises presence (+0.8), air (+0.7 from 20 %) and de-mud (+0.5 by 70 %) monotonically, ungoverned*
  - *Macros: through the chain, Width raises the side / mid ratio and Clarity lifts quiet presence-band content, both in proportion to the macro* (side / mid −12.0 / −9.8 / −7.2 dB at Width 0 / 50 / 100 %; a quiet 3.2 kHz tone +0 / +2.4 / +4.8 dB at Clarity 0 / 50 / 100 %, a loud one −2.9 dB at 100 %)
- **Device ceiling caps:** *Headset: the master limiter at the Bluetooth -2 dBTP and hands-free -3 dBTP caps holds the 4x true peak of hot inter-sample-peak material* (cap from `adviceFor()`, 44.1 / 48 / 16 / 8 kHz, ≥ 6 dB of master gain reduction: true peak ≤ cap + 0.1 dB on the 4× meter and an independent 4× interpolator, sample peak ≤ cap, no safety clamp; measured cap − 0.045 to − 0.050 dB), and the air cut-off test of §5.8.

`tests/test_modes.cpp`: the Gaming mode policy through the full chain, at least one case per Gaming macro (Footsteps: mode band 4 and band 5 lift laws and band 6 anti-masking; Positional: ILD up, mono sum unchanged, a hard-left source stays hard-left; Impact; Detail; Voice & Score: band 7), plus *Gaming: crossfeed is forced off - a hard-left source never leaks into the right ear, whatever the store says*, *Gaming: binaural lock on a 7.1 strip - width 1 and space 0 whatever the store asks, positional focus still applies* (the published effective values read width 1, space 0 and crossfeed 0) and *Gaming: a compressor switched on only by a macro is upward-only - loud sounds keep their dynamics unless a ratio was chosen*. The Positional case also checks that a hard-left 3 kHz source keeps at least 60 dB of ILD (§7.9).

`tests/test_device_profiles.cpp`: *DeviceProfiles: advice caps the ceiling per connection and warns about stacked headset DSP*, and the matching and parsing tests.

`tests/test_factory_presets.cpp`: *Factory presets: each renders a hot programme cleanly below its ceiling*, *Factory presets: Boost Intensity and all macros at 100 % stay safe*, *Factory presets: macros stack sanely and gaming presets keep positional cues*. The two render tests allow a true peak of ceiling + 0.15 dB on the 4× `TruePeakMeter` and require a sample peak at or below the ceiling and a safety-clamp count of 0. The measured margins (every preset below its ceiling on that meter, worst −1.048 dBTP for a −1 dBTP ceiling, no clamp) are in §1.4.

### 14.9 Known limitations

- **AutoDrive only reduces.** It cannot make a programme that is already louder than the target at 0 dB drive any quieter. Use `output.gain` or AutoLevel for that.
- **"No ratio chosen" means the stored ratio equals the default.** The Gaming upward-only rule (§14.4) cannot tell a stored 2.5:1 from an untouched one, so a macro-engaged compressor runs at 1:1 in both cases. To keep the 2.5:1 downward curve, switch `comp.on` on in the preset or store any other ratio (the factory-preset lint already rejects stored defaults).
- **Base distortion can hold the governor down.** The governor reacts to all measured THD+N but can only take back the governed macro contributions (§14.5). Saturation set above the budget by a preset or the user (roughly 6 dB of `sat.drive` on hot material) therefore holds the scale at its 0.3 floor, and Boost Intensity and Warmth then add only 30 % of their governed amounts. The measured distortion is published in `MeterBus::distortionDb`; the app's loudness panel shows it (floored by the clip-energy ratio) as the Distortion bar.
- **Only the saturator and the soft clipper feed the governor.** The bass harmonics generator and the clarity air exciter are measured (§14.5) but deliberately kept out of the THD+N input and meter: they add harmonics on purpose, and budgeting them would pin the governor at its floor on every harmonic-bass preset. Their combined reading is kept in `DistortionMonitor` (`getSmoothedHarmonicsDb()`) but not yet published on `MeterBus`, so the app cannot show it. Compressor and limiter gain modulation are not measured; they are covered by the limiter GR budget. The measured THD+N includes the DC that the Tube bias creates, which the 10 Hz blocker removes downstream (slightly conservative).
- **The governor's GR input is a window minimum.** It is the deepest limiting per fixed 10 ms window (so it no longer depends on the host block size: with the former per-block minimum, a drum programme at 14 dB drive held the scale at 1.0 in 64-sample blocks, 0.69 in 512 and 0.3 in 4096; now 0.74–0.76 in all three), averaged over about 3 s, so its budget refers to the deepest limiting per 10 ms, not to the mean gain. The scale itself is still updated once per host block and applied from the next block, so once the governor moves, a render changes slightly with the block size.
- **Absolute gates assume the chain's nominal level.** The loudness gates are absolute (−70 dBFS RMS, −50 LUFS) plus relative (20 LU); very quiet sources below −50 LUFS never drive the loops. The relative gate's 3 s release (§14.5) also applies within a programme: a passage more than 20 LU below the preceding material that lasts over 3 s, including a slow fade-out that spends that long in the band between the relative and the absolute gate, restarts the measurement on itself.
- **The matched bypass favours the ceiling over the match.** A raise is capped at `max.ceiling` − held dry peak, and the bypass-reference limiter then shaves what is still over, so on material with high peaks the reference can stay a little quieter than the processed output (1.2–1.5 LU in the re-measurement of §14.6). The tests assert match accuracy only where the cap is not involved (within 0.5 LU), and the ceiling where it is.

---

## 15. Chain-level CPU and latency summary

Sources: [`core/src/engine/ProcessingChain.cpp`](../core/src/engine/ProcessingChain.cpp) (`prepare()`: profile settings; latency = Σ slot latencies), [`core/src/engine/ModuleSlot.cpp`](../core/src/engine/ModuleSlot.cpp), [`core/src/engine/MixEngine.cpp`](../core/src/engine/MixEngine.cpp).

### 15.1 Latency per module and profile

Latency is the sum of the slot latencies of the current profile. It is constant until the next `prepare()` and does **not** change when a module is bypassed: `ModuleSlot` delays the dry path by the module's latency. Only the gate slot is absent outside the Quality profile. Values are in samples at 48 kHz.

| Stage | Source of latency | Quality | Balanced (default) | Low Latency |
|---|---|---|---|---|
| Input gain, AutoLevel | — | 0 | 0 | 0 |
| Virtualiser / BS.775 downmix | ITD is part of the cue (§8.6) | 0 | 0 | 0 |
| Spectral noise gate | STFT frame N (in chain only in Quality) | **1024** | — | — |
| Parametric EQ, Dynamic EQ, Bass, Clarity | minimum-phase IIR | 0 | 0 | 0 |
| Saturator | 2× oversampler round trip | 32 (2× High) | 16 (2× Low) | 16 (2× Low) |
| Stereo & Space | IIR | 0 | 0 | 0 |
| Compressor | look-ahead | 144 (3 ms) | 48 (1 ms) | 24 (0.5 ms) |
| Maximizer: clipper | oversampler round trip | 36 (4× High) | 36 (4× High) | 16 (2× Low) |
| Maximizer: limiter | look-ahead + TP detector (20) | 96 + 20 = 116 | 72 + 20 = 92 | 24 + 20 = 44 |
| **Strip total** | | **1352 = 28.17 ms** | **192 = 4.00 ms** | **100 = 2.08 ms** |

Strip totals at other rates, measured with `ProcessingChain::getLatencySamples()`:

| fs | Quality | Balanced | Low Latency |
|---|---|---|---|
| 44.1 kHz | 1332 = 30.20 ms | 182 = 4.13 ms | 96 = 2.18 ms |
| 48 kHz | 1352 = 28.17 ms | 192 = 4.00 ms | 100 = 2.08 ms |
| 96 kHz | 1592 = 16.58 ms | 312 = 3.25 ms | 148 = 1.54 ms |
| 192 kHz | 2072 = 10.79 ms | 552 = 2.88 ms | 244 = 1.27 ms |

Why the totals behave as they do:
- The oversampler round trips are fixed in samples: 16/32/36, in base-rate samples at any rate.
- The look-aheads are fixed in milliseconds.
- The gate's 1024-sample frame is fixed in samples.

**Desktop app.**
- `MixEngine` pads every strip to the largest strip latency, so relative A/V sync between applications is preserved.
- The master limiter then adds its own look-ahead plus the 20-sample detector delay: 1 ms, i.e. 68 samples = 1.42 ms at 48 kHz, or 0.5 ms (44 samples = 0.92 ms) when every strip runs the Low Latency profile, as the app sets it. App engine totals at 48 kHz: 144 samples = 3.0 ms (Low Latency), 260 = 5.4 ms (Balanced), 1420 ≈ 29.6 ms (Quality).
- The bypass-reference limiter of the global bypass (§14.5) sits inside the strip's dry-path delay and adds nothing.
- Device buffering (WASAPI / CoreAudio / ALSA periods, and the drift-compensated FIFO of captured strips) comes on top. It is not algorithmic and is not included here; the added end-to-end budget, with its scope labels (chain / app engine / added end-to-end), is `01-architecture.md` §5.
- The README's "~2.1 ms Low Latency profile" is the strip's algorithmic (chain) latency at 48 kHz.

### 15.2 CPU per module (indicative)

**How it was measured.**
- A throwaway benchmark program, not part of the repository: `flub_core` sources compiled with g++ 13.3 `-O2` and a small `main()`.
- x86-64, Intel Xeon @ 2.10 GHz in a container; 48 kHz, 512-sample blocks, stereo white noise (peak 0.3) unless noted.
- `ScopedNoDenormals` active; best of 5 runs of about 10 s of audio each.
- Two complete runs were made and the range is shown.
- The maximizer rows and §15.3 were re-measured (two more runs) after the glue-arming and limiter-hold changes. In that pass the `TruePeakMeter` reference read 76–78 ns instead of 83–87 ns, so those rows sit a few per cent low next to the others. The saturator row was re-measured after the switch to delta oversampling (§6.7; white noise of peak 0.3 and 0.5, which agree within the run-to-run spread). The Stereo & Space row was re-measured after the positional focus gained its polarity guard (§7.3.2); in the same session the previous code still read 18–21 / 23 ns.
- "% core" = ns per stereo sample ÷ 20 833 ns (one 48 kHz sample period).

These are single-machine numbers for relative comparison, not a performance specification.

| Module (configuration) | ns / stereo sample | % core | Section |
|---|---|---|---|
| Parametric EQ, 10 bands at 0 dB (defaults: skipped) | ≈ 0 | 0 % | §2.6 |
| Parametric EQ, 10 static bells | 97–101 | 0.5 % | §2.6 |
| Dynamic EQ, all bands idle / 8 active bands | ≈ 0 / 205–212 | 0 / 1.0 % | §3 |
| Bass engine, defaults (20 Hz subsonic) / all stages | 23–24 / 160–165 | 0.1 / 0.8 % | §4 |
| Clarity, neutral / all stages | 9–10 / 144–151 | 0.05 / 0.7 % | §5 |
| Saturator Tape, 2× Low / 2× High, drive 12 dB (delta oversampling) | 143–144 / 150–170 | 0.7 / 0.7–0.8 % | §6.7 |
| Stereo & Space, neutral / widened + focus + space + crossfeed | 26–27 / 31–32 | 0.12–0.13 / 0.15 % | §7.6 |
| Virtualiser, parametric, 7.1 → binaural | 78–80 | 0.38 % | §8.6 |
| Virtualiser, HRIR direct form, 7.1, 512 / 1024 taps | ≈ 1 700 / 3 340–3 450 | 8.2 / 16.5 % | §8.6 |
| Compressor, down + upward, any look-ahead | 22–23 | 0.11 % | §9.6 |
| True-peak limiter, limiting (sample-peak mode, idle / limiting) | 150–168 (11.5 / 20.5–22) | 0.8 % (0.06 / 0.1 %) | §10.6 |
| Maximizer, 4× High, clip on, glue disarmed (Balanced/Quality default) / glue armed | 367–386 / 422–431 | 1.8–1.9 / 2.0–2.1 % | §11.6 |
| Maximizer, 2× Low, clip on, glue disarmed, 0.5 ms (Low Latency default) / glue armed | 254–255 / 321–333 | 1.2 / 1.5–1.6 % | §11.6 |
| Spectral noise gate, N 1024 (Quality) | 262–279 | 1.3 % | §12.6 |
| LoudnessMeter / TruePeakMeter / LevelMeter | 8–9 / 83–87 / 8.5–9.3 | 0.04 / 0.4 / 0.04 % | §13.6 |

### 15.3 Whole-chain CPU

This is the full `ProcessingChain::process()`:
- parameter snapshot, MacroMap and mode bands;
- every slot, including the dry-delay work of bypassed ones;
- output trim and the global-bypass path;
- all meters, AutoLevel / AutoDrive / LoudnessMatch followers and analyser taps.

Stereo in and out unless noted; same conditions as §15.2. Global bypass is off in every row, so none of them includes the bypass-reference limiter (§14.5): it runs only while bypass is engaged and then costs about one true-peak limiter (150–168 ns per stereo sample, §10.6). These rows were measured before that limiter was added; with bypass off the chain skips it. They also predate the spatializer's polarity guard, which adds about 7–8 ns wherever Stereo & Space runs (§7.6).

| Scenario | Quality | Balanced | Low Latency |
|---|---|---|---|
| Defaults, Music (EQ bands skipped, dynamic EQ idle, bass subsonic, clarity neutral, spatial neutral, maximizer at 0 dB drive with clip 0.5 and glue disarmed; gate off, saturator and compressor bypassed) | 558–575 ns (2.7–2.8 %) | 554–586 ns (2.7–2.8 %) | 454–479 ns (2.2–2.3 %) |
| Music, Boost 100 % + all five macros 100 % (glue armed; Quality: gate on as well) | 1 405–1 429 ns (6.7–6.9 %) | 1 213–1 243 ns (5.8–6.0 %) | 1 047–1 093 ns (5.0–5.2 %) |
| Gaming, **7.1 input** through the virtualiser, Boost 50 % + all five macros 100 % (glue disarmed: no Gaming macro raises it) | 979–982 ns (4.7 %) | 977–991 ns (4.7–4.8 %) | 894–913 ns (4.3–4.4 %) |
| Balanced, all modules bypassed: fixed overhead of meters, control loops and bypass delay lines | — | 199–205 ns (1.0 %) | — |
| Balanced, 7.1 input, only the virtualiser active | — | 305–327 ns (1.5–1.6 %) | — |

**Reading the numbers:**
- About 200 ns per stereo sample is fixed overhead. The largest items are the 4× true-peak meter (≈ 85 ns), nine K-weighted `LoudnessFollower`s (two each for AutoLevel and AutoDrive, four for LoudnessMatch, one for input telemetry; the slow follower of each gate runs only while its gate is open) and the latency-compensating dry delays.
- At defaults the maximizer is the largest single module, because the clipper runs (clip share 0.5). Its glue splitter runs only while glue is armed (§11.3.2): in the Music boost row, not at defaults or in Gaming.
- The heaviest realistic strip, full Music boost in the Quality profile with the gate on, stays under 7 % of one 2.1 GHz core. Four strips plus the master limiter stay well inside one core.
- Profile differences come mainly from the clipper's oversampling (4× High vs 2× Low) and the gate.
- The HRIR renderer is the only configuration whose cost grows without bound, linearly in the IR length. It is not used by any host yet (§8.9).

**Headroom and roadmap.** Real-time safety depends on the worst block, not the average. The per-sample algorithms have bounded cost per block, apart from:
- the gate's FFT frame every H samples;
- the limiter's once-per-ring re-sum;
- the loudness meter's 100 ms histogram scans.

Each of these is O(N) or bounded by a small constant. Planned optimisations (**roadmap**, `docs/07-roadmap.md` item 1.9):
- SIMD across channels for the SVF cascades and the true-peak detector;
- a real FFT (PFFFT / vDSP / IPP) for the gate;
- a partitioned FFT convolver for long HRIRs (item 2.6).
