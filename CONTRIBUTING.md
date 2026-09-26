# Contributing to Flubsound Pro

## Build & test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure        # or ./build/tests/flub_tests [filter]
```

Optional targets:
- `-DFLUB_BUILD_APP=ON` builds the desktop app, and `-DFLUB_BUILD_PLUGIN=ON` the VST3 / Standalone (+ AU on macOS) plug-in. Both fetch JUCE 9.0.2; pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE` to build offline. With the app, `FLUB_BUILD_APP_TESTS` (default ON) also builds `flub_app_tests` from `tests/app/` (the app's sources with a fake audio device and a fake per-app router; `ctest` runs it, or run the binary with a filter such as `"App: AppRouting"`). A change to the device callback, `EngineController`'s device profile, the meter views or `AppRouting` should keep it green.
- `-DFLUB_SANITIZE=ON` enables ASan + UBSan (GCC / Clang; CI runs the full suite this way with Clang).
- `-DFLUB_RTSAN=ON` enables Clang RealtimeSanitizer (Clang ≥ 20; configure with `CC=clang-20 CXX=clang++-20`, not together with `FLUB_SANITIZE` or `FLUB_BUILD_PLUGIN`). It checks everything reached from the functions marked `FLUB_NONBLOCKING` (`ProcessingChain::process`, `MixEngine::process`, every `Processor::process` override); CI's `rtsan` job runs the full suite this way with `RTSAN_OPTIONS=halt_on_error=1`. A new `Processor` override needs `FLUB_NONBLOCKING` on its `process()` (declaration and definition) and a line in `tests/test_rtsan.cpp`.
- `-DFLUB_WARNINGS_AS_ERRORS=ON` is what CI uses for the Linux core builds (GCC and Clang).

## The real-time contract (non-negotiable)

Code reachable from `Processor::process()`, `reset()`, parameter setters, the device callback or `processBlock` must **not**:
- allocate or free memory (no `new`, no container growth, no `std::string`/`std::function` construction),
- lock a mutex, wait on a condition variable, or call anything that may (logging, file or network I/O, `std::cout`, most OS calls),
- throw exceptions (everything on the audio path is `noexcept`),
- run loops whose length depends on history or input content, beyond the block size and fixed design constants.

Allocate in `prepare()`. Communicate with other threads only through `ParameterStore` atomics, `MeterBus` atomics, `SpscRing`s, or single `std::atomic` values handed over once per block (as `ProcessingChain::effectiveValue()` and `AudioEngineHost`'s strip gain / ceiling hand-off do).

Every module test contains an `AllocationGuard` check. Please keep it that way.

## Style

- C++20. JUCE-like formatting: Allman braces, 4-space indent, a space before parentheses (`foo (x)`), `static_cast` for conversions. See `.clang-format`.
- Comments explain **why** (the DSP reasoning, the constraint), not what the next line does.
- File paths in core APIs are UTF-8 `std::string`s. Open files through `flub::io::pathFromUtf8()` (`core/include/flub/io/FilePath.h`), never by passing a narrow string to `std::fstream` / `std::filesystem` (on Windows that means the ANSI code page, which mangles non-ASCII names). The CLI reads its arguments and environment as UTF-8 on Windows (`tools/flubsound-cli/Utf8Windows.h`).
- Zero warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion` (GCC and Clang) and `/W4` (MSVC) on `flub_core`, the tests and the CLI (`cmake/FlubCompilerSettings.cmake`). The app and plug-in sources use a slightly lighter set because JUCE module sources compile in the same targets.

## Adversarial review checklist (every DSP change)

1. **Maths:** coefficient formulas, dB ↔ linear, time constants derived from the *current* sample rate, signs, units.
2. **Latency:** `latencySamples()` equals the measured impulse delay and never changes between `prepare()` calls.
3. **Click-freeness:** continuous parameters are smoothed; discrete changes crossfade; bypass goes through `ModuleSlot`.
4. **Block-size invariance:** identical output for blocks of 1, 7, 64 and 512 samples.
5. **Robustness:** silence, DC, full-scale noise, impulses, extreme parameters, 44.1–192 kHz, 1–8 channels all give finite, bounded output.
6. **Image safety:** dynamics are linked across channels; stereo effects preserve the mono sum where the design says so.
7. **RT safety:** the allocation test passes, and the sanitizer run is clean.
8. **Tests prove the claims:** every property stated in the header or the docs has a test with a meaningful tolerance.
9. **Docs updated:** `docs/03-dsp-design.md` (algorithm, parameters, latency, CPU) and the traceability matrix ([`docs/TRACEABILITY.md`](docs/TRACEABILITY.md)); a change to latency updates the single budget in [`docs/01-architecture.md` §5](docs/01-architecture.md#5-latency-budget).

## Presets

Factory presets live in `presets/factory/<category>-<slug>.json` and are validated by `tests/test_factory_presets.cpp`. They must:
- set only non-default parameters (string keys from `flub::param::layout()`),
- keep the maximizer on, `max.drive` at 0 and the ceiling at ≤ −1 dBTP (≤ −2 dBTP for Bluetooth presets),
- include a clear description and tags (author `Flubsound`).

The full list of checks is in `presets/README.md`.
