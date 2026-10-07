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

- **CI (GitHub Actions)** was last green at commit 714b601 (after batch 2 and the Warmth fix). CI has **not** run for
  batches 3–5: the repo went private and Actions billing blocks every job ("recent account payments have failed or
  your spending limit needs to be increased").
- **Batches 3–5 were verified only on Linux in the cloud container.**
  - Builds: gcc and clang Release with app, plug-in, tools and app tests.
  - Test runs: flub_tests and flub_app_tests, plain and under xvfb.
  - Checks: the golden step; RTSan, ASan+UBSan, the no-X11 build, the fuzzers, a MinGW cross-build of the Windows
    platform code, packaging, the offline soak and headless screenshots.
- **Never compiled on MSVC or Apple Clang so far:** anything added in batches 3–5. The risky spots are
  `Ctl.cpp`'s Winsock AF_UNIX path, the OSD's WS_EX styles and fullscreen query (SHQueryUserNotificationState), and the
  arm64 scalar path of the Enhanced virtualiser.
- **Final hand-over check (2026-09-30, Linux):** a fresh gcc Release build (app, plug-in, tools, tests, warnings as
  errors), `ctest` under xvfb (flub_tests and flub_app_tests) and the golden step (`FLUB_GOLDEN_REFERENCE=1`) all passed.
- **Real hardware:** only the per-app capture and the double-audio fix have been confirmed, on the owner's PC.

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
  prefix-filtered run but re-render everything (8.4 s / 3.0 s) when filtered on their own. Not split: in a full
  run with the machine 65–97 % busy (another job running) about 25 other core cases read 2.0–3.0 s (Startle Guard
  scenes, Music Boost 100 quality suite, Protection strength, E22 duck, intent Racing / Night Mode); they were
  under 2 s alone in the earlier measurement, but re-time them on an idle machine before deciding.
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
- **R1.1 review fixes (2026-10-07, merged):** with a programme playing, *Through Flubsound* fades the output out at its start and opens it as the first sweep arrives, so the programme's tail no longer sets the −18 dBFS cap (fake device, programme at −1.1 dBFS: cap −16.94 → 0.00 dB, sweeps −38.8 → −21.9 dBFS). *Both* hands both passes over at once, chained on the audio thread, so nothing plays between them (was 0–500 ms of programme). A Through cancel brings the programme back uncapped, and the probe pointers are loaded with seq_cst. The buffer follows the profile chosen by hand, stored as `device.bufferProfile`, so the overload response's step does not shrink it after a restart. On the first start with *Automatic buffer size*, a buffer size picked before keeps Automatic off. The back-off now doubles the size per step, with a 5 s hold. The Game strip measures 7 samples (0.15 ms) more than its reported engine latency because of the virtualiser, and the result says so. Tested with fake devices only (330/330 app tests); not on CI, not on the headset.
- **R4.5 review fixes (2026-10-07, merged):** measured here (build 26200, opt-in real check): Windows moves only streams an app opens after a per-app device change, in both directions. So an app that is playing when Flubsound puts it back (unassign / option off / quit) keeps playing to S/PDIF and, no longer captured, is not heard until it restarts its playback. Flubsound now says so (amber "Not heard" notice on the routing panel and in Settings › Routing; a log line at quit), and the texts no longer promise otherwise. Moves are now kept per executable path (two game.exe files, Discord's update folders). A crash during a re-point keeps the record ("from" in the journal). The automatic silent device skips outputs named Speakers / Headphones / Headset and any output an unassigned app plays to. The input-map line says when the device input is not processed. Return in the fix dialog is back on "Open sound settings". `canMoveAppOutput()` defaults to false (only Windows says true). Owner check still open: does Edge or a game play to the headset again after an unassign without a reload?
- **R4.4 review fixes (2026-10-07, merged):** clicking reset, the switch, Pick a free one or typed entry in Settings › Hotkeys now ends a recording first, and resuming always re-registers. Before, a chord recorded after clicking reset was saved but not registered while the page said all were registered. The recorder refuses system chords (Alt+F4, Alt+Space, Ctrl+Alt+Delete, F1-F12 alone) and, when recorded, one-modifier app chords such as Ctrl+C; typing one still takes it. Pick a free one offers only Ctrl+Alt+Shift alternatives (Ctrl+Alt+Shift+F1-F11 replace the Ctrl+Alt+letter ones, which AltGr types on); a probe on the owner's PC found them free. The row says "Could not register", the notice lists every hotkey still failing, Space / Shift+F10 / screen-reader actions reach the field, and Windows maps AZERTY and Cyrillic keys to their letter or digit key. MSVC: flub_tests pass; flub_app_tests 321 of 321 in a second ctest run (the first full run had one unrelated route-journal timing failure that then passed 5 of 5). Owner check: in the running app, click a chord, press Space and record one; click reset during a recording; check that Alt+F4 is refused.
- **R1.5 real-device soak (2026-10-07, docs/11 E53):** `Flubsound Pro --device-soak --device "<output>" [--type ...] [--buffer n|min] [--minutes m] [--report f.json] [--dump t,...] [--allow-audible]` (shell/DeviceSoak.*, docs/06 §11.1; rows: `tools/scripts/device-soak.py`, which redirects the output - the app is a windowed program) plays the full engine on one pinned output (the host only asks for that output; a device JUCE opens by itself is silenced and closed) with temporary settings (never created or written), generated game + music inside the callback and seeded automation (15 action kinds), and reports callback timing, xruns, CPU, restarts / errors, memory and the discontinuity detector's findings on the engine's output as handed to the device (device underruns are not visible; late callbacks are the proxy) with a triage class. It refuses the system default output without --allow-audible, closes its device before freeing its source and tap, and `--replay <report.json>` re-runs the session on a virtual device (a detection that comes back - same type within one block + 3 ms of the same frame - is the DSP's own; a damaged report exits 2). Soak only on "Digital Audio (S/PDIF) (High Definition Audio Device)" (silent), never the headset. Results on the owner's PC (busy with other builds; rows ran on earlier builds of the soak driver with the same real-time path): Low Latency 480 samples 10 + 30 min and shared 10 min: 0 late / over-budget callbacks, 0 dropouts in the engine's output, 0 NaN / restarts; 128 samples: 0.4 % overruns while builds ran beside it, 0.008 % in a quiet rerun; all 11 detections of rows 1-2 came back in the replay at the same frame (DC steps at the threshold on explosions, one click from the fold headroom's instant attack, E28a - owner decision), so none came from the real-time path. Review fixes (3cb3561): --dump near the start, damaged --replay reports and a stopped virtual device no longer crash or spin; the test loop gcc / clang reject is fixed. Not yet on CI (gcc / clang / Apple Clang never compiled it; clang-tidy 22's front end is clean here).
- **R5.3 neural (2026-10-07, merged; MSVC only until CI runs it):** an in-house inference runtime, TinyNet (`core/include/flub/neural/TinyNet.h`: Dense / causal Conv1D / GRU, the validated `.fnn` format with CRC and int8 rows, allocation-free `run()`), `ControlKind::BandGains` (an STFT band-gain renderer in `AsyncModelProcessor`, L = frame x (2 + safety); its circular-convolution wrap measured 56 dB under the output at worst, `tools/neural/renderer_aliasing.py`) and a first trained model, the experimental **neural voice cleanup** for the Chat strip (`VoiceCleanupRunner`, 51 k params, `presets/neural/voice-cleanup.fnn` embedded in `core/src/neural/VoiceCleanupModelData.cpp`), trained here with numpy only on 4 h of synthetic speech in noise (`tools/neural/`). Held-out synthetic set: +4.7 dB SNR and -19.9 dB between words (spectral gate +3.5 / -6.1 dB, Quality only), clean speech -0.24 dB (gate -0.04); 27 us per 5 ms frame. Off by default: Settings > Processing > Voice chat (`chat.neuralCleanup`) with a fixed-height Status line that names the fix when it cannot run; +960 samples on the Chat strip only at 48 kHz / 480 in Balanced or Quality (512-sample buffers: 25 ms, Quality only). The host resolves the safety frames for every engine it builds (`AudioEngineHost::NeuralSafety::OneDeviceBuffer`), and neural models follow their strip by name on a layout change. A paced real-time test misses 0 of 196 frames. CLI `--neural voice-cleanup`; demo pair `neural-voice-cleanup` (63 pairs; `demo --only neural-voice-cleanup`; the scene's noise sits 17.7 dB under the voice). Retrain with `python tools/neural/train_voice_cleanup.py all --work <dir> [--cli <flubsound-cli>]`; the .fnn, the embedded .cpp and `tests/neural_reference_data.h` change together. Docs: 03 §16, 09 §1.1, 11 E35 / E23, 12. Owner: listen to `neural-voice-cleanup`, then try the switch on real Discord audio.
- **E56 OSD on macOS and X11 (2026-10-07, CI-verified):** on macOS the OSD sits in its own non-activating `NSPanel` (`app/Source/ui/OsdNative_mac.mm`). It never becomes the key or main window, ignores the mouse and is set to show on every Space and over fullscreen apps. On X11 it has an empty input shape (`OsdNative_linux.cpp`): input rectangles 1 → 0, and the query is now error-checked, so a failed query reads -1, not 0. `test_app_osd.cpp` asserts the focus on all three OSes. On the macOS runner it first makes the test process the active app with a key window (`tests/app/AppTestSupport_mac.mm`); the CI line reads "this app active yes, key window the focus holder's, unchanged by the OSD". The CI Linux app job runs under Xvfb and every app job uses `ctest -V`. Runs 37580674924 (36e2c33), 37585159151 (3b27287, review fixes) and 37588059080 (ce1666e) on branch wip/macos-osd: all 11 jobs green. Not yet tried on a real Mac or a Linux desktop with a fullscreen game.
- **Merge of 2026-10-07:** the six branches above were merged with an independent audit; the device soak now turns
  *Automatic buffer size* off before its device opens (R1.5 x R1.1), the callback writes the soak tap after the latency
  probe, and the docs' stale "CI cannot run" statements are updated (commit 8fda1ff).
- A build fails at the link step (LNK1104) while `Flubsound Pro.exe` is running; the tests still build and run.

### IN FLIGHT (2026-10-07): seven requirement gaps, unmerged work in local worktree branches

The owner asked to close TRACEABILITY's "Partially implemented" rows R1.1, R1.2, R4.5, R4.4, R1.5, R5.3 and the
macOS / Linux platform gap. Each was built in its own git worktree (`.claude/worktrees/wf_ac6c1e42-cb8-N`, each with
its own `build-wt`), reviewed by an independent agent, then fixed. Items 1 and 3-7 are MERGED (8fda1ff, after a merge
audit); only item 2 (the native PipeWire backend) is still on its branch.
Branches (local; base 51abe53), status at the time of writing:

| N | Branch `worktree-wf_ac6c1e42-cb8-N` | Item | State |
|---|---|---|---|
| 1 | 47de1a6 + uncommitted fix edits | R1.1 live "Measure latency" + device buffer per profile | review fixes were in progress (uncommitted) |
| 2 | 68dea16 (also pushed as `origin/wip/pipewire-backend`) | R1.2 native PipeWire backend (dlopen) + ASIO build option | implementation iterating on CI; not reviewed yet |
| 3 | 6cb10d2 | R4.5 auto-move a captured app's own output (Windows), Settings > Routing input map (Linux) | reviewed + fixed |
| 4 | 4c91ebf | R4.4 hotkey conflicts first-class, Bypass default Ctrl+Alt+Shift+B, rebinding | reviewed + fixed |
| 5 | e828318 | R1.5 headless real-device soak (`--device-soak`, S/PDIF output) | review was in progress |
| 6 | 7ab6d26 + uncommitted fix edits | R5.3 TinyNet runtime + trained voice-cleanup model (experimental, off) | review fixes were in progress (uncommitted) |
| 7 | 3b27287 (also `origin/wip/macos-osd`) | macOS OSD as a non-activating NSPanel (CI-verified) | reviewed + fixed |

To finish: in each worktree check `git status` (commit or finish uncommitted fixes; do not delete a worktree with
uncommitted changes), build and run its tests, review anything not reviewed (items 2 and 5), then cherry-pick the
branches onto this branch one at a time (expect small conflicts in docs/TRACEABILITY.md, docs/11, docs/06, docs/12,
AppSettings / SettingsDialog), run the full ctest, push, rebuild the app. The owner still has to: run the latency
measurement (headset mic against an ear cup), decide on the ASIO SDK licence, and decide whether to install WSL
for a real Linux test.

### Next steps for the local session, in priority order

1. **Build on Windows** (commands above) and run `ctest`. Fix any MSVC-only compile errors or test failures first;
   they are the most likely breakage, because batches 3–5 were never built with MSVC.
2. **Real-hardware checks on the Turtle Beach headset**, using the list in "Testing on real hardware" above and
   docs/12 §0. Put each result in the item's docs/11 Status line, as "owner-verified", with the device name and
   connection (USB dongle / 3.5 mm / Bluetooth).
   - First: headset matching, the per-app capture, device re-plug and sleep/resume.
   - Then Warmth, Punch / Footsteps / Impact / Detail (re-voiced in batch 5), Night Mode, ChatMix and ducking.
   - Also the OSD over a fullscreen game with PresentMon, and Tournament mode with an anti-cheat game.
3. **Owner decisions.** These are listed in docs/11 §5.4, in the "Status of Phase 3 batch 4" and "Status of Phase 3 batch 5" paragraphs.
   - Release notes: 21 presets sound different after the Punch / Footsteps / Impact / Detail remap; 25 → 31 presets (Synthwave added locally);
     Late Night and Podcast are re-voiced.
   - Should Enhanced become the default virtualiser renderer?
   - Night Mode attack: 3 → 1 ms?
   - Should Relative presence be the default?
   - The E19 flux key.
   - virt.lfe +10 dB on Music.
   - Smart Loudness level.
   - Bass headroom protect.
   - E33: a one-ear HF profile turns both ears down.
   - E24 positional focus: redesign or remove?
4. **Known open items (software).**
   - Soak: the Punch decision is taken (2026-10-06, docs/11 E04 / E53): Punch's attack fades out from Boost 60 to 70 %,
     so its ticks at high Boost are gone (bit-identical below 60 %); speech ticks at medium Boost are accepted. Open:
     Boost's own maximizer ticks at full drive on loud programme (with Punch 0 too; the owner leaves them,
     2026-10-06), the bypass reference's limiter at
     high input gain and one detector false positive.
   - E28: the comb row is 18.9 dB against a < 12 dB target.
   - E11: Classical and Jazz lose 0.72 / 0.44 LU (the owner keeps the preset as voiced, 2026-10-06).
   - E22: the chat sub-limiter.
   - E07: the 2.00 dB row.
5. **Gated items** (they need hardware, people or network, not code):
   - On the Windows PC: PresentMon with the OSD, the Win/mac volume reads for the hearing guard, E51 / E16 / E55 / E22
     on hardware, and the E54 update check against a real release.
   - Linux: the desktop distribution matrix and WirePlumber 0.5.
   - Listening panels for E24, E28, E34 and E60.
   - The AMT cross-check, which needs access to sofacoustics.org.

### Re-enabling CI

Either option works:

- In GitHub › Settings › Billing and plans, fix the failed payment or raise the Actions spending limit.
- Make the repository public; Actions is free for public repositories.

Then push to the branch (or use "Re-run all jobs" on the latest run of `.github/workflows/ci.yml`).
The first green run should be recorded in `docs/TRACEABILITY.md`, `docs/04` §7 and `docs/11` §5.4. The new PipeWire
job is `continue-on-error` until it has passed 20 runs in a row.
