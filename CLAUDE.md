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

Requirements: Visual Studio 2022 with "Desktop development with C++" (includes CMake and Ninja), Git.
Run from the "x64 Native Tools Command Prompt for VS 2022" (MSVC must be on PATH; MinGW is not supported by JUCE 9):

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
