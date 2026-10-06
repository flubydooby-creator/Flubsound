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
- A build fails at the link step (LNK1104) while `Flubsound Pro.exe` is running; the tests still build and run.

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
   - Release notes: 21 presets sound different after the Punch / Footsteps / Impact / Detail remap; 25 → 30 presets;
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
     Boost's own maximizer ticks at full drive on loud programme (with Punch 0 too), the bypass reference's limiter at
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
