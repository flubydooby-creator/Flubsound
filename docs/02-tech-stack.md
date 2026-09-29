# 02 — Recommended Tech Stack & Justification (Workflow 2)

> **Summary:** C++20 everywhere.
> - A dependency-free, JUCE-free DSP core (`flub_core`) holds all audio processing.
> - JUCE 9 provides device I/O, the GUI and the plug-in wrappers.
> - A WaveRT virtual audio driver on Windows (Audio Server Plug-in on macOS, PipeWire null sinks on Linux) supplies system-wide and per-app routing. The Linux sinks exist today; the Windows driver and the macOS plug-in are designed, not yet built.
> - The build is CMake + Ninja, with GitHub Actions CI on all three platforms, an ASan/UBSan job, and allocation-counting real-time-safety tests.

---

## 1. Stack at a glance

| Layer | Choice | Main alternatives considered | Why this choice |
|---|---|---|---|
| Language | **C++20** (CI: MSVC 2022, Apple Clang 15, GCC 13, Clang 18; Clang 20 for RTSan) | Rust, C | Native to every audio SDK we must touch: WDK drivers, ASIO, Core Audio HAL plug-ins, VST3/AU. First-class JUCE support and the largest pro-audio talent pool. Deterministic performance with no GC. Modern C++ (`std::span`, `constexpr`, concepts-ready) plus sanitizers and RTSan closes most of the safety gap to Rust for our audio code. |
| DSP core | **In-house `flub_core`**, zero dependencies, no JUCE | JUCE `dsp` module, KFR, Faust-generated code | Must be hostable anywhere: app, plug-in, CLI, unit tests, and later a Windows APO or a PipeWire filter node. Keeps the AGPL/commercial JUCE licence out of the core, allows exact control over latency and allocation, and is 100 % unit-testable headless. |
| Application framework | **JUCE 9.0.2** (AGPLv3 / commercial) | Qt 6, Electron + React, Dear ImGui, native per-OS | One codebase gives audio device I/O (WASAPI shared/exclusive/low-latency, ASIO, CoreAudio, ALSA, JACK), a hardware-accelerated GUI, a system tray, accessibility, and plug-in wrappers (this project builds VST3, AU and Standalone; AAX and LV2 are also available in JUCE). Qt has no audio-plug-in or ASIO story. Electron is too heavy for a resident tray utility (see §3). |
| System-wide capture (Win) | **WaveRT virtual audio driver** (from Microsoft SYSVAD / SimpleAudioSample, MS-PL; design) + WASAPI **process loopback** (implemented) | APO injection (Equalizer APO model), third-party cables (VB-Cable, Voicemeeter) | Only a virtual endpoint per strip gives *per-application* processing with the original stream muted. APOs are per-endpoint only and fragile across Windows updates (§4.3). Third-party cables are supported as a zero-install fallback. |
| System-wide capture (mac) | **Audio Server Plug-in** (via libASPL, MIT) + **Core Audio process taps** (macOS 14.2+); both designed, not yet built | BlackHole (GPLv3), Soundflower (abandoned) | libASPL is permissively licensed. Process taps give per-app capture with *mute-when-tapped*, without a driver, on modern macOS. |
| System-wide capture (Linux) | **PipeWire** (or PulseAudio) null sinks per strip + per-app routing (`pactl` / WirePlumber rules; implemented); native `pw_filter` later | PulseAudio modules, JACK | PipeWire is the default on all mainstream distributions and routes per app natively. |
| FFT | In-house radix-2 reference (`flub::Fft`); **PFFFT** (BSD-like) or vDSP/IPP in production | FFTW (GPL / commercial), KFR (GPL / commercial) | Keeps the core dependency-free while leaving a drop-in slot for a fast SIMD FFT under a permissive licence. The GUI analyser uses `juce::dsp::FFT`. |
| Resampling | In-house drift-compensated cubic Hermite resampler (clock sync, `DriftCompensatedFifo`); **r8brain-free-src** (MIT) for offline SRC (planned; the CLI currently renders at the file's own rate) | libsamplerate (BSD-2), soxr (LGPL) | Clock-drift correction needs a tiny, RT-safe, continuously variable ratio. Offline conversion wants the highest quality. |
| File I/O | Core WAV reader/writer (CLI, tests, the app's WAV export); JUCE `AudioFormatManager` (WAV, AIFF, FLAC, Ogg, MP3 decode) and `FlacAudioFormat` (FLAC export) in the app's Export / batch process dialog | libsndfile (LGPL) | No extra dependency for the headless tools; the app gets broad format support from JUCE for free. |
| Presets / settings | JSON (in-house RFC 8259 parser in core); `juce::PropertiesFile` for app settings | nlohmann/json, TOML | Human-readable, diff-friendly, versioned, forward- and backward-compatible (string keys). |
| Build | **CMake ≥ 3.22 + Ninja**, `FetchContent` pinned JUCE tag | Projucer, Meson, Bazel | Industry standard, IDE-agnostic, works for app, plug-in, CLI and tests in one tree. |
| Tests | In-house zero-dependency framework with **allocation counting**; ASan/UBSan; Clang **RealtimeSanitizer** (CI job `rtsan`); pluginval for the plug-in (planned, roadmap item 2.9) | Catch2, GoogleTest | No network needed to build tests. Allocation counting proves the RT contract in unit tests. RTSan enforces it at run time on the `[[clang::nonblocking]]` audio entry points (`ProcessingChain::process`, `MixEngine::process`, every `Processor::process` override) over the whole unit-test suite. |
| CI | **GitHub Actions**: Linux GCC/Clang (+ sanitizers), Windows MSVC, macOS; JUCE app + plug-in builds; headless screenshots | Azure Pipelines, Jenkins | Matrix builds on all target OSes at no infrastructure cost. |
| Packaging (planned) | Windows: **WiX 4 MSI** + signed driver package; macOS: signed + notarised `.pkg`; Linux: `.deb`/`.rpm` + Flatpak | Inno Setup, MSIX, DMG-only | Drivers and HAL plug-ins need privileged installers. MSIX cannot install kernel drivers. Today only `cmake --install` exists (CLI + factory presets). |
| Crash reporting (planned) | **Crashpad** (Apache-2.0) / Sentry Native (MIT), opt-in | BugSplat, custom minidumps | Symbolicated crashes across three OSes. Opt-in to respect privacy. |
| Neural (roadmap) | **ONNX Runtime** (MIT) with DirectML / CoreML / CPU execution providers | TensorFlow Lite, LibTorch | Small runtime, hardware acceleration on every target, and a model format every framework can export. |

---

## 2. Why C++ (and not Rust) — decision record

**Context.** Real-time audio needs:
- deterministic execution with no GC pauses,
- SIMD,
- a mature ecosystem for devices, drivers and plug-in formats.

**Options.** C++20 with JUCE, or Rust with cpal, nih-plug and egui/iced/vizia.

**Decision.** C++20.

**Rationale.**
- **Drivers and OS audio SDKs are C/C++.** The WaveRT driver (WDK), the Core Audio HAL plug-in, ASIO and the WASAPI COM interfaces must be written or consumed in C/C++. Rust bindings exist for some of these but lag behind the SDKs.
- **JUCE covers four of our layers in one mature framework:** devices, GUI, tray/accessibility and plug-in wrappers. The Rust equivalents are individually good but younger. For example, cpal's exclusive-mode WASAPI and ASIO support is less complete, and Rust GUI toolkits lack JUCE's accessibility and plug-in-editor integration.
- **Hiring and maintenance.** The pro-audio engineering pool is overwhelmingly C++.

**What we do to get Rust-like safety.**
- A strict RT contract (`core/include/flub/dsp/Processor.h`), with `noexcept` on the audio path.
- Allocation-counting unit tests: every module's `process()` is proven heap-free.
- ASan + UBSan over the full suite in CI. Clang RealtimeSanitizer (`-fsanitize=realtime`, Clang 20, the `FLUB_RTSAN` build option) over the full suite in the CI job `rtsan`: `ProcessingChain::process`, `MixEngine::process`, every `Processor::process` and `Processor::reset` override and the module setters the audio thread calls are `[[clang::nonblocking]]` in that build, so an allocation, free, lock or blocking system call anywhere below them aborts the run. Host callback code is not annotated; the allocation-counting tests still cover it.
- `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (with `-Wno-sign-conversion`), with warnings as errors in the Linux CI builds.

**Revisit when** the engine is split into a separate service process (roadmap). The IPC/service layer is a good candidate for Rust because it is not bound to JUCE or driver SDKs.

---

## 3. Why JUCE for the GUI (and not Electron + React/WASM) — decision record

| Criterion | JUCE 9 native | Electron + React | Hybrid: JUCE + WebView UI |
|---|---|---|---|
| Idle RAM for a tray utility | ~40–80 MB | ~150–300 MB (Chromium + Node) | ~100–150 MB (system WebView2/WKWebView) |
| Install size | ~10–20 MB | ~100+ MB | ~20 MB (system WebView) |
| 60 fps meters/analyser | Native paint (Direct2D / CoreGraphics / OpenGL) straight from lock-free atomics | Needs an IPC bridge or shared memory → JS → canvas/WebGL | Needs a native ↔ JS bridge |
| Plug-in editor reuse | Same components in the VST3/AU editor | Not possible | Possible (JUCE WebBrowserComponent) |
| Accessibility | Built in (UIA / NSAccessibility) | Good (Chromium) | Good |
| Iteration speed on UI | Moderate | Fast | Fast for forms and pages |

**Decision.** A native JUCE UI for everything performance-sensitive: meters, analyser, EQ curve and knobs.

**Keep open.** JUCE 8 and later support WebView-based UIs with native bindings and a JavaScript support library. We may use that later for low-frequency pages: preset store, onboarding, account/cloud settings. The audio engine is JUCE-free, so this never touches DSP.

---

## 4. Platform integration choices

### 4.1 Windows (priority 1)
- **Device I/O:** JUCE's WASAPI device types:
  - *Windows Audio*: shared mode.
  - *Windows Audio (Exclusive Mode)*: lowest latency, takes the device.
  - *Windows Audio (Low Latency Mode)*: shared mode on `IAudioClient3` small periods.

  ASIO is enabled when the developer supplies Steinberg's SDK (`-DFLUB_ASIO_SDK_DIR=...`). The SDK is never committed; it is distributed by Steinberg under its own licence agreement.
- **Virtual endpoints (design, not yet built):** a WaveRT miniport driver, "Flubsound Virtual Audio", based on Microsoft's SYSVAD / SimpleAudioSample samples (MS-PL). It exposes Game (7.1), Music, Chat and System render endpoints plus a Mic capture endpoint. A private IOCTL maps the endpoint's cyclic buffer and position registers into the engine process, avoiding a second WASAPI hop. Design and the user/kernel header: `platform/windows/driver/`.
- **Per-app routing:** Windows' own per-app output device setting is honoured. Programmatic assignment needs the undocumented `IAudioPolicyConfigFactory` (`Windows.Media.Internal.AudioPolicyConfig`). It is compiled in only with `FLUB_ENABLE_UNDOCUMENTED_ROUTING` (off by default), picks the interface id by Windows build, must be validated on every supported build before release, and falls back to opening `ms-settings:apps-volume`.
- **Per-process capture without a driver:** `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (documented from Windows 10 build 20348; Flubsound uses it from build 19041, Windows 10 2004, as OBS does, and Windows 11). This captures one app's audio but cannot mute its original output, so it is a monitoring/"lite" path. The driver path is the primary one.
- **Scheduling:** MMCSS "Pro Audio" for the audio thread, and an opt-out of EcoQoS power throttling (`SetProcessInformation(ProcessPowerThrottling)`) so Windows 11 does not park the engine on efficiency cores.

### 4.2 macOS (priority 2)
- **Device I/O:** CoreAudio via JUCE. Implemented platform services: global hotkeys (Carbon `RegisterEventHotKey`), time-constraint thread policy and the output transport query (USB / Bluetooth) for headset profiles. Routing and capture below are **designs** (`platform/macos/README.md`).
- **Virtual device:** an Audio Server Plug-in (user-space HAL plug-in; no kext, no DriverKit needed) built with libASPL (MIT). We deliberately avoid GPL BlackHole code.
- **Per-app capture:** Core Audio process taps (macOS 14.2+). A `CATapDescription` with mute-when-tapped behaviour goes into a private aggregate device, so the original app output is silenced and replaced by the processed stream. Requires the audio-capture privacy permission.

### 4.3 Why not an APO as the primary Windows mechanism?
Equalizer APO proves APOs can deliver system-wide processing with near-zero added latency, because they run inside `audiodg.exe`'s own period. However:
1. They are **per endpoint**, not per application.
2. Injecting a third-party APO into another vendor's endpoint relies on registry edits that Windows feature updates, "protected" endpoints and hardware-offloaded paths have repeatedly broken.
3. Microsoft's current guidance ties APOs to the device's own driver package.

Because `flub_core` is framework-free and allocation-free, an **"APO Lite" mode** is on the roadmap for users who want zero-install-driver, per-endpoint processing. It reuses the same DSP.

### 4.4 Linux (priority 3)
- **Device I/O:** ALSA and JACK via JUCE. PipeWire serves both through its compatibility layers.
- **Virtual endpoints:** PipeWire null sinks, "Flubsound Game/Music/Chat/System" (`platform/linux/flubsound-pipewire-setup.sh`).
- **Per-app routing:** moving sink-inputs (`pactl move-sink-input`), or WirePlumber rules with `target.object`.
- **Global hotkeys:** X11 `XGrabKey` on the root window, with libX11 loaded at run time (`dlopen`, no link dependency). Wayland sessions use the xdg-desktop-portal GlobalShortcuts interface over D-Bus, with libdbus-1 loaded the same way; without that portal they report hotkeys unsupported.
- **Real-time audio thread:** best-effort `SCHED_FIFO` from the thread itself, else RealtimeKit over the system D-Bus, asked from the message thread (libdbus-1 loaded at run time; docs/11 E44). Tested against a mock rtkit only.
- **Roadmap:** a native `pw_filter` node hosting `flub_core` directly inside the PipeWire graph for the lowest latency.

---

## 5. Engine design choices (why they matter to the stack)

| Decision | Consequence |
|---|---|
| 32-bit float processing; `double` for metering integrators, biquad K-weighting, the limiter's gain envelope and running sums | Meets R1.3 while keeping LUFS/true-peak accurate to BS.1770 tolerances over hours of programme. |
| TPT state-variable filters (Cytomic/Simper) for all modulated EQ | Coefficients can change every 16 samples (dynamic EQ, smoothing) without the instability or zipper noise of direct-form biquads. |
| Linear-phase half-band FIR oversampling with **integer** round-trip latency, used as *delta* oversampling (only the nonlinearity's deviation is band-limited) | Nonlinear stages (clipper, saturation) alias far less, the top octave is not drooped, and latency compensation stays exact. Measured alias and droop figures are in `03-dsp-design.md`. |
| One 4× true-peak interpolator design (40 taps per phase, Kaiser β 5) shared by the limiter and the meters | The limiter and the meters can never disagree about a peak. |
| Lock-free parameter store with **A/B banks** | The GUI never blocks the audio thread, and A/B comparison is a single atomic bank switch. |
| Latency profiles as the only structural switch | Everything else is automatable and click-free. Latency changes only on an explicit profile change. |

---

## 6. Licensing summary (third-party components)

| Component | Licence | Use | Obligation |
|---|---|---|---|
| JUCE 9 | AGPLv3 **or** commercial JUCE 9 licence | App + plug-in | Closed-source distribution requires a commercial JUCE licence tier. |
| Steinberg ASIO SDK | Steinberg licence agreement | Optional ASIO device type | Developer obtains the SDK; not redistributed in source. |
| Steinberg VST3 SDK (inside JUCE) | Depends on the SDK version JUCE bundles: historically dual GPLv3 / proprietary Steinberg licence; recent SDK releases (VST 3.8, 2025) are MIT-licensed | VST3 plug-in | Check the bundled SDK version's licence before a closed-source VST3 release. |
| Microsoft SYSVAD / SimpleAudioSample | MS-PL | Basis of the Windows virtual driver (planned) | Keep licence notices. |
| libASPL | MIT | macOS virtual device (planned) | Attribution. |
| Crashpad / Sentry Native | Apache-2.0 / MIT | Crash reporting (planned) | Attribution (and the Apache-2.0 NOTICE file for Crashpad). |
| PFFFT | BSD-like | Production FFT (planned) | Attribution. |
| r8brain-free-src | MIT | Offline SRC (planned) | Attribution. |
| ONNX Runtime | MIT | Neural modules (planned) | Attribution. |
| HRTF datasets (SOFA) | Varies per dataset | Measured-HRIR virtualiser (the renderer is in core; a SOFA loader is planned) | Check each dataset's licence before bundling; the built-in parametric head model needs no data. |

`flub_core` itself has **no** third-party code. It can be licensed however Flubsound chooses and embedded anywhere.
