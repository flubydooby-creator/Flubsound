# Flubsound Pro — Music & Gaming Edition

**Real-time, system-wide, per-application audio enhancement for music and video games.** Louder, punchier, clearer, wider and more immersive, with distortion, clipping and latency kept under strict control.

- 🎚️ **Full processing chain:** true-peak loudness maximizer with soft clipper, 10-band parametric EQ, 4 + 4-band dynamic EQ, psychoacoustic bass engine, clarity/transient enhancer, tape/tube/digital saturation, mono-safe stereo widener/spatializer, 5.1/7.1 → binaural headphone virtualizer, look-ahead compressor (downward + upward), spectral noise gate.
- 🎮 **Gaming Mode.** Macros for footsteps, positional focus, impact, detail and voice & score. Stereo-linked dynamics protect positional cues, anti-masking keeps footsteps audible after explosions, and a **~2.1 ms** Low Latency profile serves competitive play.
- 🎵 **Music Mode.** Macros for punch, width, clarity, loudness and warmth, with dynamic de-harsh, air and de-boom companions.
- 🚀 **Boost Intensity (0–100 %).** One slider scales many modules in stages (clarity → bass → loudness). A **Safety Governor** backs it off before the limiter or clipper start to distort.
- 🛡️ **Protection:** 4× oversampled true-peak limiting (−1 dBTP), LUFS auto-level, loudness-matched A/B bypass, NaN/denormal guards, and a master limiter across all apps.
- 📊 **Pro metering:** spectrum analyzer with an interactive EQ curve, waveform/loudness history, LUFS momentary/short-term/integrated + LRA, true peak, RMS, gain reduction and correlation.
- 🎧 **Works with any headset or speakers** the OS can play to: 3.5 mm, USB, 2.4 GHz dongles, Xbox Wireless and Bluetooth, including **every Turtle Beach headset** on a PC or Mac. Device profiles recognise Turtle Beach families and apply connection-specific safety (e.g. a −2/−3 dBTP ceiling on Bluetooth) and setup advice ([docs/10](docs/10-headset-compatibility.md)).
- 🧩 **Per-app profiles and routing** via virtual endpoints: Game (7.1), Music, Chat, System. Plus a system tray, global hotkeys, factory presets, a batch CLI and a VST3/AU plug-in.

> Status: the complete engine, DSP and application foundation is implemented and tested (see `docs/07-roadmap.md`). The signed Windows virtual driver, the macOS HAL plug-in and scale QA are the remaining productisation steps.

---

## Documentation (the nine design deliverables)

| # | Document |
|---|---|
| 0 | [Understanding & delivery plan](docs/00-understanding-and-plan.md) · [Requirement traceability](docs/TRACEABILITY.md) |
| 1 | [High-level architecture & data flow](docs/01-architecture.md) |
| 2 | [Tech stack & justification](docs/02-tech-stack.md) |
| 3 | [DSP design, module by module](docs/03-dsp-design.md) |
| 4 | [Project / folder structure](docs/04-project-structure.md) |
| 5 | [Core DSP code skeletons — guided tour](docs/05-code-skeletons.md) |
| 6 | [GUI structure & components](docs/06-gui.md) |
| 7 | [Implementation roadmap](docs/07-roadmap.md) |
| 8 | [Pitfalls & concrete solutions](docs/08-pitfalls-and-solutions.md) |
| 9 | [Future expansion roadmap](docs/09-future-roadmap.md) |
| + | [Headset compatibility (incl. all Turtle Beach headsets)](docs/10-headset-compatibility.md) |

---

## Architecture in one picture

```
 Game ─┐   Music ─┐   Chat ─┐   System ─┐                     (per-app routing)
       ▼          ▼         ▼           ▼
 [Flubsound Game 7.1] [Music] [Chat] [System]   ← virtual endpoints (WaveRT driver / HAL plug-in / PipeWire sinks)
       │          │         │           │
       ▼          ▼         ▼           ▼
   ProcessingChain per strip (own profile)      ← flub_core: JUCE-free, allocation-free, lock-free
   input → AutoLevel → virtualizer → gate → EQ → dynamic EQ → bass → clarity
         → saturation → stereo/space → compressor → maximizer (glue · clipper · TP limiter)
       └──────────┴─────────┴───────────┘
                        Σ → master true-peak limiter → WASAPI / ASIO / CoreAudio / ALSA / JACK → 🎧
```

`core/` (the `flub_core` library) contains every sample-touching line of code and has **zero** third-party dependencies. The JUCE app (`app/`), the plug-in (`plugin/`), the batch CLI (`tools/`) and the tests (`tests/`) are thin hosts around it.

---

## Building

Requirements: CMake ≥ 3.22, Ninja (recommended), and a C++20 compiler (MSVC 2022, Apple Clang 15+, GCC 13+, Clang 17+).

```bash
# DSP core + unit tests + batch CLI (no external dependencies)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Desktop app and plug-in (fetches JUCE 9.0.2; use -DFETCHCONTENT_SOURCE_DIR_JUCE=... for a local copy)
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLUB_BUILD_APP=ON -DFLUB_BUILD_PLUGIN=ON
cmake --build build-app
```

Linux packages for the app build: `libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev`.
ASIO on Windows: `-DFLUB_ASIO_SDK_DIR=<path to Steinberg ASIO SDK>`.

Useful options:

| Option | Default | Meaning |
|---|---|---|
| `FLUB_BUILD_TESTS` / `FLUB_BUILD_TOOLS` | ON | Unit tests / `flubsound-cli` |
| `FLUB_BUILD_APP` / `FLUB_BUILD_PLUGIN` | OFF | JUCE desktop app / VST3 + AU + Standalone plug-in |
| `FLUB_SANITIZE` | OFF | AddressSanitizer + UndefinedBehaviorSanitizer |
| `FLUB_RTSAN` | OFF | Clang RealtimeSanitizer (Clang ≥ 20) |
| `FLUB_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` (CI uses it on Linux) |

---

## Batch processing & export (CLI)

```bash
flubsound-cli process -i song.wav -o song-enhanced.wav --preset "Punchy Pop" --boost 60 --target-lufs -12
flubsound-cli batch   -i ./album -o ./album-enhanced --mode music --boost 40 --jobs 4 --format pcm24
flubsound-cli analyze -i song-enhanced.wav            # integrated LUFS, LRA, true peak, ...
flubsound-cli params                                   # every parameter key, range and default
flubsound-cli presets                                  # factory preset list
```

Offline rendering uses exactly the same code as real-time processing. It is sample-aligned (latency compensated), and loudness targeting iterates until the output is within 0.3 LU of the target while the true-peak ceiling holds.

---

## Repository layout

```
core/        flub_core — primitives, DSP modules, analysis, engine, presets & file I/O (no JUCE)
tests/       zero-dependency unit tests (allocation-free proofs, response/latency/ceiling properties)
app/         JUCE desktop app — engine host, GUI, tray, hotkeys, platform services
plugin/      VST3 / AU / Standalone wrapper around the same ProcessingChain
tools/       flubsound-cli — batch processing, export, loudness analysis
platform/    virtual-device designs & scripts (Windows WaveRT driver, macOS HAL, Linux PipeWire)
presets/     factory presets (JSON) — Music, Gaming, Device
docs/        the design deliverables
```

Details: [docs/04-project-structure.md](docs/04-project-structure.md).

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). It covers the real-time contract, style and the adversarial review checklist.

## Licensing notes

`flub_core` contains no third-party code. The app and plug-in use JUCE, which is licensed under AGPLv3 or a commercial JUCE licence; a closed-source release needs a commercial JUCE licence. Other third-party obligations are listed in [docs/02-tech-stack.md §6](docs/02-tech-stack.md).
