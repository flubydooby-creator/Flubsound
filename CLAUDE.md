# Flubsound Pro — notes for Claude Code

Flubsound Pro — Music & Gaming Edition: a real-time, system-wide audio enhancer.
It is written by **Flubes** (the owner) and **Claude**, co-authors (see AUTHORS.md).
The product is called **Flubsound**; never use any other name.

## Layout

- `core/` — `flub_core`, the C++20 DSP engine (no third-party dependencies, real-time safe).
- `app/` — the JUCE 9 desktop app "Flubsound Pro"; `plugin/` — "Flubsound FX" (VST3 / AU / Standalone).
- `tools/flubsound-cli/` — offline renders, `quality`, `analyze`, `demo` (before/after pairs), `latency-probe`.
- `tests/` — `flub_tests` (core) and `tests/app/` — `flub_app_tests`; golden files in `tests/golden/`.
- `docs/` — design docs; `docs/11-enhancement-report.md` is the roadmap (E01–E60, each with Status lines);
  `docs/TRACEABILITY.md` maps requirements to code and tests.
- `installer/` — Windows Inno Setup script, macOS dmg script, Linux install script.

## Building on Windows (the owner's PC)

Requirements: Visual Studio 2022 or 2026 with "Desktop development with C++" (includes CMake and Ninja), Git.
The owner's PC has Visual Studio 2026 Community (MSVC 19.51). Run from the "x64 Native Tools Command Prompt"
(or call `VC\Auxiliary\Build\vcvars64.bat` first; MSVC must be on PATH; MinGW is not supported by JUCE 9):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLUB_BUILD_APP=ON -DFLUB_BUILD_PLUGIN=ON -DFLUB_BUILD_TESTS=ON -DFLUB_BUILD_TOOLS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- App: `build\app\FlubsoundPro_artefacts\Release\Flubsound Pro.exe`
- Plug-in: `build\plugin\FlubsoundFX_artefacts\Release\VST3\`
- Installer (optional, needs Inno Setup 6): `bash tools/scripts/package-desktop.sh build dist windows` from Git Bash.
- The first configure downloads JUCE (FetchContent), so it needs internet.

## Testing on real hardware (what only the owner's PC can do)

The owner tests on Windows 11 with Turtle Beach headsets. Real-device behaviour has never been checked
anywhere else, so these are the highest-value checks (see docs/11 §5 "gated" items):

- Per-app capture (routing panel) and the double-audio fix: an app captured per process must have its
  own output moved to another device (Settings › System › Sound › Volume mixer), or it is heard twice.
  Confirmed working on the owner's PC; the amber "original also audible" badge should flag it.
- Headset profile matching: Settings › Audio / the headset panel should name the Turtle Beach series.
  Record the exact Windows endpoint names in `tests/data/endpoint-names-corpus.txt` if one is missed.
- Headset on-board enhancement cap (Superhuman Hearing on → Footsteps / Detail capped, virtualiser off).
- Dongle re-plug into another USB port, hot-plug, sleep/resume (E51), Tournament mode with anti-cheat (E55).
- Warmth (Music macro 5): 0 → 100 should sound warmer (more 200 Hz body, softer highs) at equal loudness.
- `flubsound-cli demo --out demo` renders loudness-matched before/after WAV pairs with an index.txt.

Report findings in `docs/11` Status lines (owner-verified, with the device name and connection).

## Conventions

- Real-time rules: no allocation, locks or I/O on the audio thread; new audio-thread entry points are
  `FLUB_NONBLOCKING` and listed in `tests/test_rtsan.cpp`.
- A sound change needs before → after numbers (KnownGap tests, `tools/scripts/preset-render-diff.py`);
  golden files in `tests/golden/` are re-recorded only deliberately, once per change set.
- Every test case stays under 2 s; warnings are errors in CI (`-DFLUB_WARNINGS_AS_ERRORS=ON`).
- Development branch: `claude/optimistic-ride-wvaohl`. A cloud session may also be pushing to it; pull
  before working and prefer a separate branch (for example `local/<topic>`) for local changes.
- Every commit ends with these trailers:
  `Co-Authored-By: Flubes <flubydooby@gmail.com>` followed by the Claude co-author trailer.

## Hand-over from the cloud session (2026-09-30)

### Current state

- docs/11 Phases 0–2 are done. Phase 3 ran in five batches, and the cloud session stopped after batch 5 at the owner's request.
  Each E-item's Status line in `docs/11-enhancement-report.md` says what is done, what remains and what is gated.
- `docs/12-feature-guide.md` is the Feature & Listening Guide. For every control it says what it does to the sound and how
  to check it by ear; the checks use `flubsound-cli demo`, A/B and the module ear.

### What was verified where

- **CI (GitHub Actions) runs again; the repository is public.** Billing blocked every job up to run 36695058369 (the
  hand-over commit 8e5a11c). Jobs start again from run 36783725945 (2026-09-30), but core failed on Apple Clang (from
  2026-10-01 on gcc too), so the app jobs were skipped until c2d05e5: its run 37427019405 had every core leg and the
  Linux app job green (the Windows and macOS app jobs failed). The Windows app job went green after caccf81
  (37444883054), the macOS one after 51abe53. **First fully green run after the hand-over: 37572308549** (51abe53,
  2026-10-07, all 11 jobs).
  Six of the items merged on 2026-10-07 (R1.1, R1.5, R4.4, R4.5 / R4.6, R5.3, E56) first ran together in 37598813718
  (055e792); all seven, with R1.2, first ran together in 37615196405 (b9d9ab7: the macOS core leg failed the paced
  VoiceCleanup test, the app, `asio` and `pipewire` jobs were skipped). On this branch the first run in which all their jobs ran is
  37634884865 (70a2a98): 11 of 12 green, the non-blocking `pipewire` job failed on an idle xrun (docs/11 E48). The
  code at HEAD (unchanged since 3554497) ran all 12 jobs green, `asio` included, in 37649158870 on `wip/pipewire-xrun` (the `wip/mac-neural` iterations on top of b9d9ab7 had run all
  12 jobs from 37617044054 and were all green in 37624502399).
  The branch's own run on that commit, 37654557630, ended as a failure in its first attempt, a GitHub-side failure to
  create the dependent jobs: its 7 core, sanitizer, RTSan and fuzz jobs were all green, the app, `asio` and `pipewire`
  jobs were never created (no failed step or error); its re-run (attempt 2) ran all 12 jobs green.
- **Batches 3–5 are built and tested on CI:** MSVC (windows-2022), gcc and clang (ubuntu-24.04), Apple Clang
  (macos-14, arm64), RTSan, ASan + UBSan and the fuzzers; app, plug-in, `flub_app_tests` and pluginval on all three
  OSes; the gcc golden-render step; the `pipewire` job. Locally on MSVC 19.51 too (step 1 below). The former risky
  spots (`Ctl.cpp`'s Winsock AF_UNIX path, the OSD's WS_EX styles and fullscreen query, the Enhanced virtualiser's
  arm64 scalar path) build and pass there. Until then batches 3–5 had run only on Linux in the cloud container.
- **Real hardware (owner-verified, Windows 11, Turtle Beach Stealth 600PC Gen 3 on its USB dongle):** headset matching
  (E16); per-app capture (Edge onto the Music strip) with the double-audio fix; recovery after a dongle re-plug into
  another USB port, headset off / on and sleep / wake (E51); by ear on the Music strip: Warmth, Width, Clarity,
  Loudness, Late Night, Podcast & Voice, A/B, the contour following the Windows volume, the Punch ticks on speech
  at Boost ~70 % (E53); after the high-Boost fade, music (The Veldt) at Boost 100 + Punch 100 "sounds a little
  better" (E04). **Run by the local session on the owner's PC** (not owner-verified, not on the headset): the R1.5
  device soak (S/PDIF output), the R4.5 router check (build 26200) and the R4.4 hotkey probe.

### Local session progress (Windows, from 2026-09-30)

- **Step 1 done.** First MSVC build (Visual Studio 2026, warnings as errors): app, plug-in, tools and tests build;
  `ctest` passes. Only three test files needed fixes (`getenv` → `_dupenv_s`, one unused X11 guard).
- **E51 owner-verified (2026-10-06):** dongle moved to another USB port and the headset switched off / on during per-app
  capture, and sleep / wake: the sound came back by itself every time.
- **Owner listening (2026-10-06, Music strip via Edge):** Warmth (warmer, same loudness), Width, Clarity, Loudness,
  Late Night, Podcast & Voice, A/B and the contour's Windows-volume follow all work; Classical & Jazz stays as voiced.
- **E16 owner-verified:** the owner's headset is a **Turtle Beach Stealth 600PC Gen 3** on its USB dongle; Windows
  names it "Speakers (Stealth 600PC Gen 3)", which matched nothing until a `stealth 600pc` token was added.
- **E34 done:** a preset's `smart` flag is applied by the app, the plug-in and the CLI (see the docs/11 E34 Status).
- **The 2 s rule:** the Scenes matrix is one case per row and level (34 cases) and the factory hot-programme and
  full-macros checks one case per preset (registered like the intent cases). On Windows two more causes were found:
  the core runner slept at 15.6 ms (no JUCE, so no `timeBeginPeriod (1)`; TestMain now calls it — NeuralSlot 13 s →
  ~1 s per case) and Windows file I/O made the WAV fuzz 17 s (now `readWavMemory`, the same parser, < 0.5 s).
  The cases that were still 2.0–7 s alone on the owner's PC (MSVC) are now split, assertions unchanged, each new
  name the old one plus " - <row>" (so docs references still match by substring); the printed measurements are
  identical before and after. Core: governed macros at 64..4096-sample blocks (one case per block, 1.2–1.6 s, plus
  the spread check over their memo), step/bed contrast (Footsteps + bell and Competitive FPS per level, ≤ 1.5 s),
  DriftFifo settling (per drift × packet, ≤ 0.65 s), the Chain sample-rate sweep (per rate, ≤ 0.8 s), the quality
  suite's meta-validation (4 cases, ~0.7 s), the maximizer at 12 dB on the quality suite (clipper default / off,
  ~1.2 s), Scenes metric validation (bypass per level, bell + cut, pump; ≤ 1.0 s) and the harmonics policy (2 cases,
  ~1 s). App: Voice Chat at -35 / -12 LUFS (per level, 1.3 / 1.6 s, plus the within-3-LU check over their memo),
  First Run - Game pink beds (3 cases, ~0.75 s), and AppRouting's outdated-pass case (2.05 → 0.05 s: it now wakes
  the worker for one more pass, so the flickering session's parity no longer waits out the 2 s refresh).
  Caveats: the two memo comparison cases (governed spread, Voice Chat within 3 LU) take < 0.01 s in a full or
  prefix-filtered run but re-render everything (8.4 s / 3.0 s) when filtered on their own. The ~25 core cases
  that read 2.0–3.0 s in a full run were **Windows power throttling of the runner**, not slow cases (2026-10-08):
  Windows 11 stops honouring the 1 ms timer of a process with no visible window and may run it under EcoQoS. Both
  runners now opt out like the app does (`SetProcessInformation` in `tests/TestMain.cpp`, the app's
  `SystemTuning::disablePowerThrottling` in `tests/app/AppTestMain.cpp`; 09dc1a4). Throttled, 2 of 4 full core runs
  also failed the paced VoiceCleanup case (64 worker wake-ups per second = the 15.6 ms timer, 70 of 196 frames
  missed). After: full core run 1161 / 1161 in 257 s (386 s throttled), no case at 2 s (slowest 1.66 s); app suite
  378 / 378 in 38.9 s, slowest 1.26 s. Nothing needs a split. Time cases with the runner as is (per-case times:
  stream its `[ RUN ]` / `[ OK ]` lines with timestamps); a run from a background script is now representative.
- **Soak click triage:** the EQ's discrete-change crossfade is now a smoothstep (2 of the 12 clicks, a preset switch);
  the other 10 are explained in docs/11 E53.
- **E20 automatic preamp:** the preamp's model counts Gaming Impact's LF burst (Impact 100: preamp 0.00 → −6.22 dB;
  no factory preset turns the preamp on, so nothing in the renders or golden files moves).
  Smart's bass multiplier now scales the burst as well (no factory preset turns Smart on).
  The burst's harmonics are now reserved under `bass.protect` with the lift: the 7 Impact presets' kick renders
  move (cleaner; their rows in both golden files re-based from MSVC deltas, to be confirmed by a Linux gcc re-record).
- **E04 step 5:** Quality sets Clarity's 1 ms look-ahead (Quality 1352 → 1400 samples at 48 kHz; the plug-in's PDC
  follows). The six Quality presets' rows in `tests/golden/preset-render-baseline.json` were re-based from an MSVC
  before → after diff; a Linux gcc re-record should confirm them.
- **Synthwave preset (owner request 2026-10-06):** a new Music preset for synthwave / synth-pop / progressive house
  (30 → 31 presets; demo pair `preset-synthwave`, 61 → 62 pairs). Its golden entry and render-baseline rows are MSVC
  values; a Linux gcc golden run should confirm them. Not heard yet (docs/11 E14 Status).
- **Analyser optional views (owner request 2026-10-06):** the default Spectrum + EQ look is unchanged; a View menu adds
  a hover readout (note + cents, levels), Diff (out − in), Sharper lows (8 kHz-decimated long FFT below 300 Hz),
  Spectrogram, Stereo width (new `AnalyzerTaps::postSide` ring), Freeze and Piano keys, persisted in `ui.analyzer`
  (docs/06 §6.4.1, docs/12 §8, `tests/app/test_app_analyzer_views.cpp`). Not yet seen on the owner's screen.
- **Analyser visualisers (owner request 2026-10-06):** a framework (`app/Source/ui/vis/`: `Visualiser` interface,
  `VisualiserRegistry.cpp` list, `VisualiserHost`) for optional views in place of / beside the spectrum or as a strip,
  chosen in View › Visualiser and persisted in `ui.analyzer` (13 fields; older values still read). Six views:
  goniometer, stereo field by frequency, correlation strip, loudness history, waveform before / after, gain-reduction
  history. The post side tap became `AnalyzerTaps::postStereo` (aligned mid / side pairs). Adding a view = new files +
  one registry line (docs/06 §6.4.2, docs/12 §8, `tests/app/test_app_visualisers.cpp`, screenshot states `vis-<id>`).
  Not yet seen on the owner's screen.
- **Eye-candy visualisers (owner request 2026-10-06):** a 3D waterfall (`waterfall-3d`: 6 s of the output spectrum as a
  hidden-line landscape) and a radial spectrum (`radial-spectrum`: mirrored ring with a bass pulse and the momentary
  LUFS), plus a visualiser window (View › *Open in a window*: any main view or the spectrum / spectrogram, F11 / Esc
  borderless full screen, second monitor, persisted in `ui.visualiserWindow`, off in Tournament mode) (docs/06 §6.4.2,
  docs/12 §8, `tests/app/test_app_visualiser_window.cpp`). Not yet seen on the owner's screen or at 60 fps on real music.
- **Music-theory views (owner request 2026-10-06):** a multi-pitch estimator (`vis/PitchEstimator.*`: 16 kHz-decimated
  0.51 s Blackman FFT, harmonic summation over C1–C8 with iterative subtraction, one bass note below C3) and
  `vis/MusicTheory.*` (chord naming incl. slash chords, a steadying chord tracker, Krumhansl-Schmuckler key over a
  15 s window). Piano keys can light only the fundamentals (View › *Piano keys: fundamentals only*, persisted as
  keys field 2 / 3 in `ui.analyzer`); three visualisers: *Chord name* (`chord`, main / strip), *Chromagram*
  (`chromagram`, main / strip, 15 s history, key in the header) and *Song key* (`key`, strip) (docs/06 §6.4.2,
  docs/12 §8, `tests/app/test_app_music_views.cpp`). Checked only on synthetic tones and the app's own test music
  (Am-F-C-G with drums: Am, F, C read, A minor); not yet heard / seen on real songs on the owner's screen.
- **R1.1 review fixes (2026-10-07, merged):** with a programme playing, *Through Flubsound* fades the output out at its start and opens it as the first sweep arrives, so the programme's tail no longer sets the −18 dBFS cap (fake device, programme at −1.1 dBFS: cap −16.94 → 0.00 dB, sweeps −38.8 → −21.9 dBFS). *Both* hands both passes over at once, chained on the audio thread, so nothing plays between them (was 0–500 ms of programme). A Through cancel brings the programme back uncapped, and the probe pointers are loaded with seq_cst. The buffer follows the profile chosen by hand, stored as `device.bufferProfile`, so the overload response's step does not shrink it after a restart. On the first start with *Automatic buffer size*, a buffer size picked before keeps Automatic off. The back-off now doubles the size per step, with a 5 s hold. The Game strip measures 7 samples (0.15 ms) more than its reported engine latency because of the virtualiser, and the result says so. Tested with fake devices only (330/330 app tests; green on CI since run 37624502399, on this branch since 37634884865); not on the headset.
- **R4.5 review fixes (2026-10-07, merged):** measured here (build 26200, opt-in real check): Windows moves only streams an app opens after a per-app device change, in both directions. So an app that is playing when Flubsound puts it back (unassign / option off / quit) keeps playing to S/PDIF and, no longer captured, is not heard until it restarts its playback. Flubsound now says so (amber "Not heard" notice on the routing panel and in Settings › Routing; a log line at quit), and the texts no longer promise otherwise. Moves are now kept per executable path (two game.exe files, Discord's update folders). A crash during a re-point keeps the record ("from" in the journal). The automatic silent device skips outputs named Speakers / Headphones / Headset and any output an unassigned app plays to. The input-map line says when the device input is not processed. Return in the fix dialog is back on "Open sound settings". `canMoveAppOutput()` defaults to false (only Windows says true). Owner check still open: does Edge or a game play to the headset again after an unassign without a reload?
- **R4.4 review fixes (2026-10-07, merged):** clicking reset, the switch, Pick a free one or typed entry in Settings › Hotkeys now ends a recording first, and resuming always re-registers. Before, a chord recorded after clicking reset was saved but not registered while the page said all were registered. The recorder refuses system chords (Alt+F4, Alt+Space, Ctrl+Alt+Delete, F1-F12 alone) and, when recorded, one-modifier app chords such as Ctrl+C; typing one still takes it. Pick a free one offers only Ctrl+Alt+Shift alternatives (Ctrl+Alt+Shift+F1-F11 replace the Ctrl+Alt+letter ones, which AltGr types on); a probe on the owner's PC found them free. The row says "Could not register", the notice lists every hotkey still failing, Space / Shift+F10 / screen-reader actions reach the field, and Windows maps AZERTY and Cyrillic keys to their letter or digit key. MSVC: flub_tests pass; flub_app_tests 321 of 321 in a second ctest run (the first full run had one unrelated route-journal timing failure that then passed 5 of 5). Owner check: in the running app, click a chord, press Space and record one; click reset during a recording; check that Alt+F4 is refused.
- **R1.5 real-device soak (2026-10-07, docs/11 E53):** `Flubsound Pro --device-soak --device "<output>" [--type ...] [--buffer n|min] [--minutes m] [--report f.json] [--dump t,...] [--allow-audible]` (shell/DeviceSoak.*, docs/06 §11.1; rows: `tools/scripts/device-soak.py`, which redirects the output - the app is a windowed program) plays the full engine on one pinned output (the host only asks for that output; a device JUCE opens by itself is silenced and closed) with temporary settings (never created or written), generated game + music inside the callback and seeded automation (15 action kinds), and reports callback timing, xruns, CPU, restarts / errors, memory and the discontinuity detector's findings on the engine's output as handed to the device (device underruns are not visible; late callbacks are the proxy) with a triage class. It refuses the system default output without --allow-audible, closes its device before freeing its source and tap, and `--replay <report.json>` re-runs the session on a virtual device (a detection that comes back - same type within one block + 3 ms of the same frame - is the DSP's own; a damaged report exits 2). Soak only on "Digital Audio (S/PDIF) (High Definition Audio Device)" (silent), never the headset. Results on the owner's PC (busy with other builds; rows ran on earlier builds of the soak driver with the same real-time path): Low Latency 480 samples 10 + 30 min and shared 10 min: 0 late / over-budget callbacks, 0 dropouts in the engine's output, 0 NaN / restarts; 128 samples: 0.4 % overruns while builds ran beside it, 0.008 % in a quiet rerun; all 11 detections of rows 1-2 came back in the replay at the same frame (DC steps at the threshold on explosions, one click from the fold headroom's instant attack, E28a - owner decision), so none came from the real-time path. Review fixes (3cb3561): --dump near the start, damaged --replay reports and a stopped virtual device no longer crash or spin; the test loop gcc / clang reject is fixed. Built and tested in CI's three app jobs since run 37624502399 (on this branch since 37634884865).
- **R5.3 neural (2026-10-07, merged; green on CI since run 37624502399, on this branch since 37634884865, with the macOS worker fix 70a2a98):** an in-house inference runtime, TinyNet (`core/include/flub/neural/TinyNet.h`: Dense / causal Conv1D / GRU, the validated `.fnn` format with CRC and int8 rows, allocation-free `run()`), `ControlKind::BandGains` (an STFT band-gain renderer in `AsyncModelProcessor`, L = frame x (2 + safety); its circular-convolution wrap measured 56 dB under the output at worst, `tools/neural/renderer_aliasing.py`) and a first trained model, the experimental **neural voice cleanup** for the Chat strip (`VoiceCleanupRunner`, 51 k params, `presets/neural/voice-cleanup.fnn` embedded in `core/src/neural/VoiceCleanupModelData.cpp`), trained here with numpy only on 4 h of synthetic speech in noise (`tools/neural/`). Held-out synthetic set: +4.7 dB SNR and -19.9 dB between words (spectral gate +3.5 / -6.1 dB, Quality only), clean speech -0.24 dB (gate -0.04); 27 us per 5 ms frame. Off by default: Settings > Processing > Voice chat (`chat.neuralCleanup`) with a fixed-height Status line that names the fix when it cannot run; +960 samples on the Chat strip only at 48 kHz / 480 in Balanced or Quality (512-sample buffers: 25 ms, Quality only). The host resolves the safety frames for every engine it builds (`AudioEngineHost::NeuralSafety::OneDeviceBuffer`), and neural models follow their strip by name on a layout change. A paced real-time test misses 0 of 196 frames. CLI `--neural voice-cleanup`; demo pair `neural-voice-cleanup` (63 pairs; `demo --only neural-voice-cleanup`; the scene's noise sits 17.7 dB under the voice). Retrain with `python tools/neural/train_voice_cleanup.py all --work <dir> [--cli <flubsound-cli>]`; the .fnn, the embedded .cpp and `tests/neural_reference_data.h` change together. Docs: 03 §16, 09 §1.1, 11 E35 / E23, 12. Owner: listen to `neural-voice-cleanup`, then try the switch on real Discord audio.
- **E56 OSD on macOS and X11 (2026-10-07, CI-verified):** on macOS the OSD sits in its own non-activating `NSPanel` (`app/Source/ui/OsdNative_mac.mm`). It never becomes the key or main window, ignores the mouse and is set to show on every Space and over fullscreen apps. On X11 it has an empty input shape (`OsdNative_linux.cpp`): input rectangles 1 → 0, and the query is now error-checked, so a failed query reads -1, not 0. `test_app_osd.cpp` asserts the focus on all three OSes. On the macOS runner it first makes the test process the active app with a key window (`tests/app/AppTestSupport_mac.mm`); the CI line reads "this app active yes, key window the focus holder's, unchanged by the OSD". The CI Linux app job runs under Xvfb and every app job uses `ctest -V`. Runs 37580674924 (36e2c33), 37585159151 (3b27287, review fixes) and 37588059080 (ce1666e) on branch wip/macos-osd: all 11 jobs green. Not yet tried on a real Mac or a Linux desktop with a fullscreen game.
- **Merge of 2026-10-07:** the six branches above were merged with an independent audit; the device soak now turns
  *Automatic buffer size* off before its device opens (R1.5 x R1.1), the callback writes the soak tap after the latency
  probe, and the docs' stale "CI cannot run" statements are updated (commit 8fda1ff).
- **R1.2 audio backends (2026-10-07, merged after the six above; Linux code CI-verified only):** the native PipeWire device type opens `libpipewire-0.3.so.0` at run time (`pipewire/PipeWireLibrary.*`: dlopen and a dlsym table; the app links no libpipewire, CI checks there is no NEEDED entry and runs the app with the library hidden), so one binary starts without PipeWire and keeps ALSA / JACK. A first start prefers the node only when PipeWire plays the audio (a server answers and has an output sink that is not Flubsound's, `pipewire::serverPlaysAudio`), so a PulseAudio desktop keeps ALSA (pulse-alsa). The node counts its own xruns (`XrunCounter`: a cycle finished late or cycles missed; the first two cycles of a run, after a restart or a new driver are not judged, and the device takes its baseline at its first callback) for the header's `xr` and the overload watchdog, stamps each block with the driver's time for the E45 timing, reports a removed node or a lost server to the host's E51 recovery directly (JUCE 9.0.2 drops `audioDeviceError`; a test pins the gap) and drops a stale node error on close. Final CI run 37605340907 (branch wip/pipewire-backend, all 12 jobs green): no start-up xrun in 20 of 20 loops (before 4-9 of 20), wall clock 2.6664-2.6671 ms mean against 2.667 ms. ASIO is a build option: `-DFLUB_ASIO=ON` with `FLUB_ASIO_SDK_DIR` (an SDK you downloaded) or `FLUB_ASIO_FETCH` (the official SDK 2.3.4 archive, SHA-256 pinned, used under its GPLv3 option; `cmake/FlubAsio.cmake`), off by default; CI's non-blocking `asio` job builds and tests it and uploads nothing. Settings > Audio shows a **DEVICE TYPE** note for ASIO and Windows Audio (Exclusive Mode) (single client; the Windows way in for other apps: per-app capture with the app's own output moved, a virtual cable on exclusive mode's input, or a shared type) and for PipeWire, right under the device selector (so the selector never moves) and above R1.1's LATENCY panel (`tests/app/test_app_device_types.cpp` checks both places). Owner decision: the ASIO licence for a published build (docs/02 §6: Steinberg's proprietary licence with a signed agreement, or GPLv3 for the whole build). Not yet tried: a real ASIO driver, exclusive mode with the per-app way in, a Linux desktop or real sound card. MSVC here: flub_app_tests 378/378, full ctest passes.
- A build fails at the link step (LNK1104) while `Flubsound Pro.exe` is running; the tests still build and run.

### Merged 2026-10-07

The seven requirement items are merged into this branch: R1.1 latency measurement + automatic buffer, R1.2 PipeWire
native + ASIO option, R1.5 real-device soak, R4.4 hotkey conflicts, R4.5 move-away routing + R4.6 input map, R5.3
TinyNet + experimental neural voice cleanup, E56 macOS / X11 OSD (merge fixes 8fda1ff, 975afb9). After the merge:
the neural worker's macOS scheduling (70a2a98) and the PipeWire xrun case that attributes an idle xrun to a
scheduling stall (d4f05a5, 5e04bac, 3554497). The worktrees `.claude/worktrees/wf_ac6c1e42-cb8-1..7` are kept for
reference; nothing on them is left to merge. The temporary remote branches `wip/macos-osd`, `wip/pipewire-backend`,
`wip/pipewire-xrun`, `wip/mac-neural`, `wip/mac-neural-check`, `wip/mac-neural-diag` and `wip/mac-neural-diag2`
(CI iterations) were deleted on 2026-10-08 with the owner's OK.

### Merged 2026-10-08

Five open software items (workflow wf_7df3eb45-bb4: implement, independent review, fix; each had all 12 CI jobs
green on its own `wip/` branch) are merged as an implementation commit and a review-fix commit each (cd98552 ..
1835002), with no textual conflict. An independent audit found no code problem (the E22 numbers are unchanged on
the merged tree, though its bypassed chains now pass E53's smooth take-over) and stale or false docs statements
(CI state, the soak known gap, docs/12 §13), all fixed. Locally the full ctest passes (flub_tests 1161,
flub_app_tests 378) and the strict render diff reads 0 of 5121 with the merged CLI; **the merged code (1835002) ran
all 12 CI jobs green in run 37741568844**.

- **E59 render-diff baseline on CI.** CI's gcc `core` leg runs `tools/scripts/preset-render-diff.py --strict
  --largest 20 --print-moved` against `tests/golden/preset-render-baseline.json` ("Preset render diff (reference
  platform)", about 30 s, **blocking** like the golden renders). `--strict` fails a render when a value moves by more
  than 0.02 dB / LU or 0.02 points (governor scale 0.002), the latency changes, or a render is added or missing;
  without it the script keeps its 0.1 dB default for local diffs. A probe on every core leg (run 37722477794) read
  0 of 5121 values different on gcc 13.3, clang 18.1, MSVC 19.44 and Apple Clang 15 (arm64), MSVC 19.51 locally too,
  so the 48 MSVC-derived rows (E20 Impact, E04 step 5 Quality, Synthwave) were already the gcc values; nothing was
  re-based. **A change that moves a factory preset's render, even below the tolerance, re-records the baseline
  (`--update`; MSVC is fine) in the same change**; `--print-moved` prints every moved line in the CI log. The golden
  renders, the render diff and the demo pack run whenever the build succeeded.
- **E28 comb row met.** The Enhanced renderer references each speaker's delays to its nearer ear: comb 18.88 →
  9.38 dB (< 12); Classic is bit-identical (render diff 155/155 at 0 dB). A renderer switch relearns the level match
  (7.1 correlated pink: +2.96 dB in the first 0.5 s and still +1.00 dB 2.5 s later → within 0.34 dB from 0.5 s on).
  Trade: the far ear of a same-side pair combs deeper at 1.3 kHz (FL + SL right ear −25.1 → −37.8 dB, only 4-7 dB
  under the near ear); the ILD at 16 kHz is up to −3.25 dB off the analytic sphere (Classic −1.78 dB). Not heard.
- **E22 chat room inside the duck.** With *Duck game under voice chat* on, a gain after the Game strip's ceiling
  limiter leaves room for the chat under the master ceiling, never deeper than the floor
  (`ChatDucker::kDefaultRoomFloorDb` −6 dB, kept by the owner 2026-10-08; `MixEngine::setChatRoomFloorDb`, 0 dB = off; the app does
  not set it yet). A silent chat leaves the offset ceiling alone, exactly. A −14 LUFS teammate under −1 dBFS
  explosions: chat drop 0.82 → 0.45 / 0.18 / 0.15 dB at floors −6 / −9 / −12 dB (game peaks pulled down 4.7 / 7.4 /
  10.0 dB); 0 clicks with the Game strip at 0 to +12 dB; +7 to +11 ns per sample. The duck limiter's release no
  longer stalls at −0.0019 dB (the stage idles again). Synthetic signals only.
- **E53 soak follow-ups.** The bypass reference's limiter takes over smoothly (`LimiterEnvelope::smoothTakeover`,
  only the chain's `dryLimiter`): loud scene at +10 / +16 / +22 dB input gain 7 / 12 / 9 → 0 clicks; nothing moves
  with bypass off. The user-gaming 97.05 s "false positive" is real: a one-sample step from the positional-focus
  guard's instant attack (owner decision). The detector's broadband check is opt-in (`--band-check`; it would hide a
  break made ahead of an EQ high cut such as Lo-fi Chill's 10 kHz). The soak programme is now the same on every
  compiler (clang drew its random numbers in another order). `soak.py --minutes 2`: user rows 14 → 5 clicks, 0 while
  bypassed.
- **R5.3 / E35 neural worker woken by the audio thread.** `AsyncModelProcessor::process()` signals a `WakeEvent`
  (SetEvent / futex / Mach semaphore; that one OS call is a documented RTSan exemption) instead of the worker
  polling: wake-ups about 1 500 → 100 per second on macOS / Linux, about 500 → 100 on Windows; idle 7-10 per second;
  worker CPU on macOS 28-32 → 10-12 ms per second; `process()` up to about 20 µs higher. Polling stays as the
  fallback; its paced test twin prints its misses instead of bounding them. No sound change. Not yet on a real Mac.

The worktrees `.claude/worktrees/wf_7df3eb45-bb4-1..5` are kept for reference; nothing on them is left to merge.
New temporary remote branches: `wip/golden-gcc`, `wip/e22-room`, `wip/neural-wake`, `wip/neural-wake-print` (a
print-only CI step, never merged), `wip/e53-bypass-limiter`, `wip/e28-comb`; all deleted on 2026-10-08 with the owner's OK.

### IN FLIGHT (2026-10-09): the 3D brain visualiser (owner request), unmerged

Owner request: a 3D brain view of how the music travels through the brain, based on real neurology. The owner
approved a three.js browser mock-up after four rounds (3D point-cloud brain he loves; connected nerve tracts;
no orbs; landing spots that brighten and grow with intensity, dark when quiet). Workflow wf_0f5beb3e-d6e builds it
as the analyser visualiser "brain" (CPU rendering like Waterfall3D; ascending auditory pathway with tonotopy and
real relative latencies shown 20x slower, contralateral dominance, beat network, chord-surprise IFG, dopamine
caudate / accumbens; approximate MNI coordinates) in a workflow worktree, branch pushed as `wip/brain-view`, then two
reviews, a fix pass and a verification. To finish if interrupted: find the worktree with `git worktree list`
(branch name from the workflow journal), check `git log 93eb318..HEAD` there, finish or review, merge onto this
branch, full ctest, push, rebuild the app; the owner said "commit when ready". The mock-up and its screenshots:
the session scratchpad's `brain3d_v5.html` and `brain-shots/`.

### Next steps for the local session, in priority order

1. **Build on Windows: done** (step 1 above; CI first fully green on all three OSes in run 37572308549; latest
   all-green code: 37741568844, the 2026-10-08 merge).
2. **Real-hardware checks still open** on the Stealth 600PC Gen 3 (rebuild the app from HEAD first; docs/12 §0).
   Put each result in the item's docs/11 Status line, as "owner-verified", with the device name and connection.
   - Games: Punch / Footsteps / Impact / Detail (re-voiced in batch 5), Night Mode, ChatMix and ducking, the on-board
     enhancement cap (Superhuman Hearing), the OSD over a fullscreen game with PresentMon, Tournament mode with an
     anti-cheat game (E55).
   - R1.1: Settings › Audio › *Measure latency* with the headset mic against an ear cup.
   - Listening / looking: Synthwave (not heard yet), the analyser views and visualisers (not yet seen on the owner's
     screen; 60 fps on real music), the music-theory views on real songs.
   - R5.3: the `neural-voice-cleanup` demo pair, then the switch on real Discord / chat audio.
   - E28: the `enhanced-renderer` demo pair, then a 7.1 game's footsteps moving between side and front; in the app
     wait about a second after switching the renderer before comparing.
   - R4.5: the move-away with Edge: does Edge (or a game) play to the headset again after an unassign without a reload?
   - R4.4: record a chord, reset during a recording, check that Alt+F4 is refused.
3. **Owner decisions** (docs/11 §5.4, the batch 4 / 5 paragraphs, and the Status lines).
   - Decided 2026-10-06: Punch fades out from Boost 60 to 70 % (speech ticks at medium Boost accepted, E04 / E53);
     Classical & Jazz stays as voiced (−0.72 / −0.44 LU, E11); Boost's own maximizer ticks at full drive are left.
   - Decided 2026-10-08: **no Linux testing yet** - do not build or test Flubsound in the PC's WSL Ubuntu (it is
     installed, and another project builds in it) until the owner says so; Linux stays CI-only. The E22 room floor
     stays at −6 dB (the engine default, so the app needs no setting; −9 dB was the alternative). The `wip/*` CI
     branches are deleted (all 13, after checking that their work is on this branch).
   - Release notes: 21 presets sound different after the Punch / Footsteps / Impact / Detail remap; 25 → 31 presets
     (Synthwave added locally); Late Night and Podcast are re-voiced; *Automatic buffer size* is new and on by default,
     and a buffer size picked before keeps it off (E42c).
   - Should Enhanced become the default virtualiser renderer?
   - Night Mode attack: 3 → 1 ms?
   - Should Relative presence be the default?
   - The E19 flux key.
   - virt.lfe +10 dB on Music.
   - Smart Loudness level.
   - Bass headroom protect.
   - E33: a one-ear HF profile turns both ears down.
   - E24 positional focus: redesign or remove?
   - New (2026-10-07): the ASIO licence for a published build (Steinberg's proprietary licence with a signed
     agreement, or GPLv3 for the whole build; docs/02 §6); a real Linux test in WSL (not yet, see above); the buffer back-off's step (now at least double the size per step, 5 s hold, thresholds untuned; E42c);
     the E28a fold headroom's instant attack (one soak click on a full-scale 7.1 explosion; a soft attack or a short
     look-ahead would move only renders of such overs; E53); the neural voice cleanup model (experimental, off,
     trained on synthetic speech only): keep, retrain or hide?
   - New (2026-10-08): E22: should the room also hold the game's own excess with the Game fader up (extra limiting
     while the duck is in), and a loud-teammate-over-explosions demo pair to judge it by ear; E28: accept the far
     ear's deeper 1.3 kHz comb and the ~0.5 s settle after a renderer switch (listen to `enhanced-renderer` first),
     and should E28a's virt on / off overs row count as met only when the default renderer meets it (Classic
     −1.91 LU, Enhanced −0.56 LU)?; E53: soften the positional-focus guard's instant attack (the user-gaming 97.05 s
     step)?; E35: keep the polling twin of the paced VoiceCleanup test (about 1 s per run) or drop it?; E59: the strict
     render diff blocks CI and the golden renders / demo pack also run after an earlier failed step - OK?
4. **Known open items (software).**
   - Soak: R1.5 on a real device: captures, device inputs and the drift FIFO, a 128-sample run on an idle machine
     (on the merged code with the PC 93 % busy from another project's WSL builds: 48 late / 2 over budget of
     225 004 callbacks, 0 clicks / dropouts, docs/11 E53 2026-10-08; was 896 / 837 with builds, 16 / 19 at 78 %);
     the maximizer's full-drive clicks and the untriaged user-music 107.79 s / user-fps 44.56 s clicks (E53). (The
     bypass reference limiter and the detector item are done, 2026-10-08.)
   - E28: the comb row is met (9.38 dB, 2026-10-08); left: Enhanced's far-ear ILD at 16 kHz (a better far-ear
     interpolator, its own change).
   - Float release stall: HeadphoneVirtualizer's fold headroom and FoldHeadroom (E28a, `Bs775Fold.h`) release like
     the old duck limiter and stall at about −0.0019 dB after an over (fixed for the duck in E22); the fix moves
     renders, so it needs its own change with render diffs and a baseline re-record.
   - E22: the room is done (2026-10-08); its floor stays at the engine default −6 dB (owner, 2026-10-08).
   - E07: the 2.00 dB row.
   - Golden: the MSVC-recorded rows pass CI's gcc golden-render step (±0.05 dB) since run 37427019405 and, since
     2026-10-08, the blocking strict render diff of `tests/golden/preset-render-baseline.json` (0 of 5121 values
     different); nothing is left to re-record.
   - Tests that draw random numbers inside one argument list get other inputs per compiler (they pass everywhere):
     `tests/test_compressor.cpp` (~line 1434), `tests/test_parametric_eq.cpp` (~line 1021).
   - PipeWire on CI: the job is blocking since 2026-10-08; an idle xrun is tolerated only as an attributed
     scheduling stall; a desktop with real-time priority is not checked (E48).
5. **Gated items** (they need hardware, people or network, not code):
   - On the Windows PC: PresentMon with the OSD, the Win/mac volume reads for the hearing guard, E55 / E22 on
     hardware, and the E54 update check against a real release.
   - A real Mac: the OSD over a fullscreen game's Space (E56), the neural worker (os_workgroup, wake-up energy; E35).
   - Linux: the desktop distribution matrix, WirePlumber 0.5, the R1.2 node on a real desktop and sound card.
   - Listening panels for E24, E28, E34 and E60.
   - The AMT cross-check, which needs access to sofacoustics.org.

### CI

CI runs on every push. Billing blocked every job on 2026-09-29 / 30 ("recent account payments have failed or your
spending limit needs to be increased"); the repository is now public, where Actions is free. Record first green runs
in `docs/TRACEABILITY.md` (verification baseline), `docs/04` §7 and `docs/11` §5.4. The `asio` job (it depends on
Steinberg's server) is `continue-on-error`. The `pipewire` job was so until 20 runs in a row were green; it is
**blocking since 2026-10-08** (streak 37643504114 .. 37744936845, docs/11 E48). `fuzz` has a 20 min timeout (a slow
apt mirror took the old 10 min in the install alone in run 37744936845, which then needed a re-run).
Count on 2026-10-07: 27 green `pipewire` results (26 runs on all branches plus one re-run; 7 on this branch) and 5
failures (2 on WIP R1.2 code, a test-comment compile error on `wip/mac-neural`, the idle xruns of 70a2a98 and
d4f05a5); the longest green streak is 12 runs, the current one 3 (37643504114, 37649158870, 37654557630 attempt 2).
Count on 2026-10-08: 16 more green `pipewire` results and no failure (the five items' `wip/` runs, this branch's
37661923343, 37721890037 and the merge's 37741568844; cancelled or skipped jobs not counted), so 43 green and 5
failures in all, and the current streak is 19 (37643504114 .. 37741568844), the longest so far. Then 37744936845
(9cc4a7e) made it 20 (44 green), and the job became blocking.
