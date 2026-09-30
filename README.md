# Flubsound Pro — Music & Gaming Edition

**Real-time, system-wide, per-application audio enhancement for music and video games.** Louder, punchier, clearer, wider and more immersive, with distortion, clipping and latency kept under strict control.

- 🎚️ **Full processing chain:** true-peak loudness maximizer with soft clipper, 10-band parametric EQ, 4 + 4-band dynamic EQ, psychoacoustic bass engine, clarity/transient enhancer, tape/tube/digital saturation, mono-safe stereo widener/spatializer, 5.1/7.1 → binaural headphone virtualizer, look-ahead compressor (downward + upward), spectral noise gate.
- 🎮 **Gaming Mode.** Macros for footsteps, positional focus, impact, detail and voice & score. Stereo-linked dynamics protect positional cues, anti-masking keeps footsteps audible after explosions, and a Low Latency profile serves competitive play: **~2.1 ms** chain latency at 48 kHz, 3.0 ms for the desktop app's engine including its master limiter, and an estimated ≈ 9.5 ms added end to end, inside the 10 ms target (Balanced, the default, is ≈ 12–13 ms). End-to-end figures are estimates until the loopback measurement on the roadmap; per-app capture paths add their own buffering ([latency budget](docs/01-architecture.md#5-latency-budget)).
- 🎵 **Music Mode.** Macros for punch, width, clarity, loudness and warmth, with dynamic de-harsh, air and de-boom companions.
- 🚀 **Boost Intensity (0–100 %).** One slider scales many modules in stages (clarity → bass → loudness). A **Safety Governor** backs it off when the limiter works too hard or the measured distortion (THD+N) of the saturator and the clipper exceeds its budget; the Boost panel says what it is doing and why, and a protection strength (Off / Normal / Strict) lets it also govern a preset's own drive.
- 🛡️ **Protection:** true-peak limiting on a 4× interpolated detector (−1 dBTP default), LUFS auto-level, loudness-matched bypass plus A/B parameter banks (the A/B, the "listen without this module" buttons and a blind A/B/X test are loudness matched too, so the louder setting cannot win a comparison), NaN/denormal guards, and a master limiter after the strip sum in the desktop app. A **Dynamic Range** control holds sudden explosions and gunfire to a set amount over the scene before them, **Smoothness** takes back the harsh "s" sounds that enhancement adds, and a **loudness contour** adds the bass and treble the ear misses at low volume (all three off unless a preset sets them; Night Mode Gaming uses Dynamic Range); in the desktop app the contour can follow the system volume against a reference volume you set (Settings › Processing, off by default).
- 🪟 **Simple and Advanced views:** the window opens in a Simple view with the mode, preset, Boost, the five macros, Dynamic Range and Smoothness, what is active now, your headset and one loudness meter; one click shows the Advanced view with routing, spectrum and EQ, the module rack (ordered by what matters in the current mode) and every meter, and the app remembers the last view. Every control's tooltip says in one sentence what you will hear. The window fits laptop screens down to 800 × 560, and a click on the tray icon opens quick controls (Boost, preset, Bypass).
- 📊 **Pro metering:** spectrum analyzer with an interactive EQ curve, waveform/loudness history, LUFS momentary/short-term/integrated + LRA, true peak, RMS, gain reduction and correlation, plus what the strip is doing now: "active now" chips for the stages that change the sound, the in → out loudness difference and how often the limiter works.
- 🔎 **Preset browser:** click the preset name to search the presets by name, tag or description, filter them by mode, category, tag or what suits your headset, star favourites and find recent ones, and hear a preset on the strip before you load it; the louder of the preview and your current sound is turned down to the other, so a preset does not win the comparison by being louder. Cancel puts your sound back.
- 🔔 **Clear warnings:** a muted or failed output device shows a banner with Retry, Choose output and Sound settings (a virtual cable you monitor on purpose can be allowed in Settings › Audio); a preset with unknown or out-of-range settings, a restored settings file and a preset made for another latency profile show a notice.
- 🎧 **Works with any headset or speakers** the OS can play to: 3.5 mm, USB, 2.4 GHz dongles, Xbox Wireless and Bluetooth, including **every Turtle Beach headset** on a Windows, macOS or Linux computer. Device profiles recognise Turtle Beach families and apply connection-specific safety (a −2 dBTP ceiling on Bluetooth A2DP, −3 dBTP in hands-free mode) and setup advice ([docs/10](docs/10-headset-compatibility.md)).
- 🧩 **Per-app profiles and routing:** four strips (Game 7.1, Music, Chat, System), each with its own profile. They are fed by PipeWire null sinks on Linux, by per-process loopback capture on Windows 10 version 2004 (build 19041)+ / Windows 11, or by any virtual cable. Plus a system tray, global hotkeys (Windows, macOS, Linux under X11), factory presets, a batch CLI and a VST3/AU/Standalone plug-in.

> Status: the complete engine, DSP and application foundation is implemented and tested (see `docs/07-roadmap.md`). The remaining productisation steps are the signed Windows virtual driver and the macOS HAL plug-in with process-tap capture (both designed in `platform/`, not yet built), and scale QA.

![Flubsound Pro in Gaming mode with the headset advice banner](docs/images/app-gaming-headset-advice.png)

*Rendered headlessly by the app (executable `Flubsound Pro`): `--screenshot out.png --mode gaming --size 1440x900 --device "Headphones (Stealth 700 Gen 2 MAX)"`; `--device` simulates the output device. More screenshots are in `docs/images/` (`app-music.png`, `app-gaming.png`, `app-bluetooth-handsfree-1100x700.png`).*

---

## Installing a test build

Every CI run of the `app` job uploads an unsigned test build per OS (Actions → the run → Artifacts). Start with the headset volume low. `TESTING.txt` inside each package has the details.

| Artifact | What is in it | How to install |
|---|---|---|
| `Flubsound-windows` | `FlubsoundPro-Setup-<version>.exe` and the plain `Flubsound-windows` folder | Run the installer: the app goes into Program Files, the Flubsound FX VST3 into `C:\Program Files\Common Files\VST3`, with a Start menu entry and an uninstaller (Settings › Apps). Or start `Flubsound Pro.exe` from the folder. SmartScreen warns (More info → Run anyway) |
| `Flubsound-macos` | `Flubsound-macos.dmg` and `Flubsound-macos.zip` (the same files) | Open the `.dmg` and drag Flubsound Pro onto Applications; copy the plug-ins from `Plug-ins/` to `~/Library/Audio/Plug-Ins/VST3` and `/Components`. First start: right-click → Open |
| `Flubsound-linux` | `Flubsound-linux.tar.gz` | Unpack, then run `./Flubsound Pro`, or `./install.sh` for a menu entry, `~/.local/bin/flubsound-pro` and the VST3 in `~/.vst3` (`./install.sh --uninstall` removes them) |

To report a problem, use *Settings › Diagnostics › Export diagnostics*: one zip with the log (device changes, errors, glitch and overload counts; no audio), any crash reports, the settings and the device details, with your home folder, login and computer name replaced.

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
| 11 | [Post-construction enhancement report](docs/11-enhancement-report.md) |
| 12 | [Feature and listening guide](docs/12-feature-guide.md): every control, what it does to the sound in numbers, what to listen for, its demo-pack pair and a by-ear checklist |

---

## What each control does (one-screen summary)

Every control, with its numbers, what to listen for and how to compare it fairly, is in the [feature and listening guide](docs/12-feature-guide.md). `flubsound-cli demo --out demo` renders a loudness-matched before / after pair for each one (61 pairs; `--input my-song.wav` for your own music), and the guide ends with a checklist to fill in by ear.

| Control | What you should hear |
|---|---|
| **Boost Intensity** | Music: clarity and width first, then bass, loudness last. Gaming: detail and direction, then impact. Watched by the Safety Governor |
| **Punch** · **Width** · **Clarity** · **Loudness** · **Warmth** (Music) | harder drum starts · a wider image that survives mono · clearer voice and cymbals · denser and louder · a softer top and fuller body |
| **Footsteps** · **Positional** · **Impact** · **Detail** · **Voice & Score** (Gaming) | steps lifted as they happen · sharper placement · a bigger thump on blasts · quiet sounds closer · dialogue and score over the effects |
| **Dynamic Range** | sudden blasts held to 20 / 15 / 10 / 6 LU over the scene before them |
| **Smoothness** | the "s" sounds that the enhancement adds taken back |
| **Smart macros** (Settings › Processing) | less attack and drive on masters that are already loud |
| **Module cards** (Advanced view) | EQ, dynamic EQ, bass engine, clarity, saturation, stereo and crossfeed, the 7.1 headphone virtualiser, compressor, maximizer styles; hold a card's ear to hear the strip without it |
| **ChatMix** · **Duck game under voice chat** | the game quieter against chat · the game dipped around 1–2.4 kHz only while a teammate talks, footsteps kept |
| **Headset enhancement is ON** | Footsteps and Detail capped at 30 % so Superhuman Hearing and Flubsound do not stack |
| **Settings › Hearing** | an estimate of your listening level and dose (with your headset's sensitivity), an optional level cap, and a per-ear profile |
| **A / B**, **Bypass**, the module ear, **Blind test** | comparisons at matched loudness, so the louder side cannot win |

---

## Architecture in one picture

```
 Game ─┐   Music ─┐   Chat ─┐   System ─┐                     (per-app routing)
       ▼          ▼         ▼           ▼
 [Flubsound Game 7.1] [Music] [Chat] [System]   ← virtual endpoints (PipeWire sinks today; WaveRT driver / HAL plug-in designed)
       │          │         │           │         or per-app loopback capture (Windows) / a virtual cable
       ▼          ▼         ▼           ▼
   ProcessingChain per strip (own profile)      ← flub_core: JUCE-free, allocation-free, lock-free
   input gain → AutoLevel → virtualizer / downmix → gate → EQ → dynamic EQ → bass → clarity
         → saturation → stereo/space → compressor → maximizer (glue · clipper · TP limiter)
       └──────────┴─────────┴───────────┘
                        Σ → master true-peak limiter → WASAPI / ASIO / CoreAudio / ALSA / JACK → 🎧
```

`core/` (the `flub_core` library) contains every DSP module, the meters and the engine, and has **zero** third-party dependencies. The JUCE app (`app/`), the plug-in (`plugin/`), the batch CLI (`tools/`) and the tests (`tests/`) are thin hosts around it; host-side signal code is limited to I/O glue and visualisation (the app's drift-compensating capture FIFO, the GUI's analyser FFT, and a test-signal generator for headless screenshots).

---

## Building

Requirements: CMake ≥ 3.22, Ninja (recommended), and a C++20 compiler. CI builds with MSVC 2022 (windows-2022), Apple Clang 15 (macos-14), GCC 13 and Clang 18 (ubuntu-24.04), plus Clang 20 for the RealtimeSanitizer job.

```bash
# DSP core + unit tests + batch CLI (no external dependencies)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Desktop app and plug-in (fetches JUCE 9.0.2; use -DFETCHCONTENT_SOURCE_DIR_JUCE=... for a local copy)
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLUB_BUILD_APP=ON -DFLUB_BUILD_PLUGIN=ON
cmake --build build-app
ctest --test-dir build-app --output-on-failure   # flub_tests + the app-level flub_app_tests: no audio device or display needed
```

Linux packages for the app build: `libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev` (CI also installs `libgl1-mesa-dev`, and `xvfb` for the headless screenshots; `libxi-dev` is optional and enables JUCE's XInput2 support).
Linux at run time: create the `flubsound_*` sinks with `platform/linux/flubsound-pipewire-setup.sh install`. Per-app routing needs `pactl` (pulseaudio-utils 16+). On PipeWire, the app links each strip sink's monitor to its input by itself, which needs `pw-dump` / `pw-link` (`pipewire-bin` / `pipewire-utils`); without them it says so on stderr, and qpwgraph does the same by hand. It also asks for a 256/48000 quantum (`PIPEWIRE_LATENCY`, 5.3 ms instead of pipewire-jack's 21.3 ms default) unless you set the variable yourself. Details: [platform/linux/README.md](platform/linux/README.md).
ASIO on Windows: `-DFLUB_ASIO_SDK_DIR=<path to Steinberg ASIO SDK>`.

Useful options:

| Option | Default | Meaning |
|---|---|---|
| `FLUB_BUILD_TESTS` / `FLUB_BUILD_TOOLS` | ON | Unit tests / `flubsound-cli` |
| `FLUB_BUILD_APP` / `FLUB_BUILD_PLUGIN` | OFF | JUCE desktop app / VST3 + Standalone plug-in (+ AU on macOS) |
| `FLUB_BUILD_APP_TESTS` | ON | With `FLUB_BUILD_APP`: also build `flub_app_tests` (`tests/app/`), the app-level tests of the device callback, headset ceiling cap, meters and per-app routing |
| `FLUB_SANITIZE` | OFF | AddressSanitizer + UndefinedBehaviorSanitizer (GCC / Clang only) |
| `FLUB_RTSAN` | OFF | Clang RealtimeSanitizer (Clang ≥ 20 required, else configure fails): the audio entry points (`ProcessingChain::process`, `MixEngine::process`, every `Processor::process` and `Processor::reset` override, the module setters the audio thread calls) become `[[clang::nonblocking]]` and abort on allocation, locks or blocking calls. CI job `rtsan` runs the full test suite this way |
| `FLUB_BUILD_FUZZERS` | OFF | libFuzzer targets in `tests/fuzz` (Clang with libFuzzer only; not with `FLUB_RTSAN`): the JSON parser, preset load and migration, saved strip / plug-in state and device profiles, ParametricEQ text; run them with `tests/fuzz/run-fuzzers.sh <build> <seconds>`. CI job `fuzz` runs each for 30 s |
| `FLUB_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` (CI uses it for the Linux core builds) |
| `FLUB_JUCE_VERSION` | `9.0.2` | JUCE git tag fetched for the app / plug-in |
| `FLUB_ASIO_SDK_DIR` | empty | Windows app: Steinberg ASIO SDK root; enables the ASIO device type |
| `FLUB_FACTORY_PRESET_DIR` | `presets/factory` | Folder whose `*.json` files the app embeds as factory presets |
| `FLUB_ENABLE_UNDOCUMENTED_ROUTING` | OFF | Windows app: lets the routing panel move apps between output devices through the undocumented `IAudioPolicyConfigFactory` API (see `docs/08` D7) |
| `FLUB_WITH_PIPEWIRE` | ON | Linux app: build the native PipeWire node and registry linking when `pkg-config` finds libpipewire-0.3 (`libpipewire-0.3-dev`); without it the app falls back to `pw-dump` / `pw-link` (docs/11 E48) |

---

## Batch processing & export (CLI)

The build above puts the CLI at `build/tools/flubsound-cli/flubsound-cli` (`flubsound-cli.exe` on Windows; with a multi-config generator such as Visual Studio, under a `Release/` or `Debug/` sub-folder). The examples assume it is on your `PATH`: either call it by that path, or install it with `cmake --install build --prefix <dir>`, which puts it in `<dir>/bin` and the factory presets in `<dir>/share/flubsound/presets/factory`.

```bash
flubsound-cli process -i song.wav -o song-enhanced.wav --preset "Punchy Pop" --boost 60 --target-lufs -12
flubsound-cli batch   -i ./album -o ./album-enhanced --mode music --boost 40 --jobs 4 --format pcm24
flubsound-cli analyze -i song-enhanced.wav            # integrated LUFS, LRA, true peak, ...
flubsound-cli quality --mode music --boost 100         # THD+N, IMD, MTND, ducking, kick timing on pinned stimuli
flubsound-cli quality --macro warmth=100 --rate 44100  # ... plus aliasing, DC and ultrasonic energy at 44.1 kHz
flubsound-cli params                                   # every parameter key, range and default
flubsound-cli presets                                  # factory preset list
flubsound-cli demo --out demo                          # loudness-matched before / after pairs of every feature (docs/12)
```

Offline rendering uses exactly the same `ProcessingChain` as real-time processing. It is sample-aligned (latency compensated). Loudness targeting re-renders up to 4 more times, moving `max.drive` (then `input.gain` to go louder, or `output.gain` and then `input.gain` to go quieter) until the integrated loudness is within 0.3 LU of the target, and delivers the closest pass; the true-peak limiter holds the ceiling, with a static trim as a last resort. The CLI reads WAV only (PCM 16/24/32-bit, float 32/64, `WAVE_FORMAT_EXTENSIBLE`, up to 8 channels) and writes float32, or PCM24 / PCM16 with TPDF dither. The printed and `--json` output report measures the file as written: a PCM export is read back, so quantisation and dither are included. `tests/test_offline_render.cpp` runs the render, export and batch code (including parallel jobs and a corrupt input file).

### In the desktop app

Preset menu (the `…` next to the preset box) › **Export / batch process audio files…** does the same without a terminal: add files or a folder (with or without sub-folders, or drop them on the dialog), choose an output folder, a format (WAV float32 / PCM24 / PCM16, FLAC 24 / 16-bit), the settings (the selected strip's current settings, or any preset) and optionally a loudness target and a true-peak ceiling, then **Start**. It reads WAV, AIFF, FLAC, Ogg Vorbis and MP3 (through JUCE), renders on a background thread with the CLI's own offline renderer (a WAV export is byte-identical to `flubsound-cli process` with the same settings), and lists each file's loudness in and out, true peak and status; **Cancel** stops after the current file and **Reveal in folder** shows the result. It never writes next to its inputs: an output folder that is an input folder is refused. Details: [`docs/06-gui.md` §6.12](docs/06-gui.md#612-exportdialog--export--batch-process); tests: `tests/app/test_app_export.cpp`.

---

## Repository layout

```
core/        flub_core — primitives, DSP modules, analysis, engine, neural-model framework, presets & file I/O (no JUCE)
tests/       zero-dependency unit tests (allocation-free proofs, response/latency/ceiling properties)
app/         JUCE desktop app — engine host, GUI, tray, hotkeys, platform services
plugin/      VST3 / AU / Standalone wrapper around the same ProcessingChain
tools/       flubsound-cli — batch processing, export, loudness analysis; scripts/ (device-profile embedding, preset render diff, quality report)
platform/    virtual-device designs & scripts (Windows WaveRT driver design, macOS HAL design, Linux PipeWire sinks)
installer/   test-build packaging: Windows Inno Setup script, macOS .dmg script, Linux menu entry and install.sh
presets/     factory presets (JSON) — Music, Gaming, Device
docs/        the design deliverables
```

Details: [docs/04-project-structure.md](docs/04-project-structure.md).

---

## Authors

Flubsound Pro is written by **Flubes** ([@flubydooby-creator](https://github.com/flubydooby-creator)), co-author and project owner, and **Claude** (Anthropic), co-author. See [AUTHORS.md](AUTHORS.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). It covers the real-time contract, style and the adversarial review checklist.

## Licensing notes

`flub_core` contains no third-party code. The app and plug-in use JUCE, which is licensed under AGPLv3 or a commercial JUCE licence; a closed-source release needs a commercial JUCE licence. Other third-party obligations are listed in [docs/02-tech-stack.md §6](docs/02-tech-stack.md).
