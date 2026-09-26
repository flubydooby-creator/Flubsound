# 07 — Implementation Roadmap: MVP → Advanced → Polish (Workflow 5)

This roadmap covers the path from this repository to a 1.0 release on Windows, macOS and Linux. Durations assume the team in §1. Every phase ends with **exit criteria**: measurable gates, not feature lists.

**Where we are today.** Phase 0 is delivered in this repository:
- The complete DSP core: every module, the engine, meters, presets and I/O.
- Unit tests with real-time-safety proofs.
- The JUCE desktop app, plug-in and CLI.
- Platform integration scaffolding, factory presets and CI.

What remains is mainly **productisation**:
- the signed virtual driver,
- per-app routing hardening,
- macOS/Linux integration,
- UX polish and scale testing.

---

## 1. Team & assumptions

| Role | FTE | Focus |
|---|---|---|
| Lead DSP engineer | 1 | Chain, dynamics, loudness, tuning, presets |
| DSP engineer | 1 | Spatial / HRTF, bass, clarity, noise reduction, neural (later) |
| Systems / driver engineer (Windows) | 1 | WaveRT driver, signing, WASAPI, routing, installers |
| C++ / JUCE GUI engineer | 1 | App, UI components, UX, accessibility |
| macOS + Linux engineer | 0.5 → 1 (Phase 3) | HAL plug-in, process taps, PipeWire |
| QA / audio test engineer | 1 | Device lab, soak tests, listening tests |
| Product designer | 0.5 | UX, visual design, onboarding |
| Product / project manager | 0.5 | Scope, beta programme, release |

Calendar estimates include roughly 20 % contingency. Weeks are counted from project start.

---

## 2. Phase overview

| Phase | Weeks | Theme | Outcome |
|---|---|---|---|
| **0 — Foundations** | 1–3 | Architecture, contracts, core DSP, CI | ✅ Done (this repository) |
| **1 — MVP** | 4–13 | Windows desktop app on a single strip via a virtual cable | Closed beta: 200 users |
| **2 — Advanced** | 14–29 | Own virtual driver, per-app routing, gaming spatial, plug-in | Public beta (Windows) |
| **3 — Polish** | 30–41 | macOS + Linux, UX polish, performance, localisation, 1.0 | 1.0 on Windows + macOS; Linux beta |
| **4 — Post-1.0** | 42+ | Neural features, integrations | See `09-future-roadmap.md` |

```
Weeks:        0    5    10   15   20   25   30   35   40   45
Phase 0      ███
Phase 1         ██████████
Phase 2                   ████████████████
Phase 3                                   ████████████
Driver sign.              ░░░░░░░░ (attestation) ░░░░ (WHQL)
Beta                         [closed]      [public]      [1.0]
```

---

## 3. Phase 0 — Foundations (weeks 1–3) ✅

| Work item | Status | Evidence |
|---|---|---|
| Requirements, traceability, delivery plan | ✅ | `docs/00-understanding-and-plan.md`, `docs/TRACEABILITY.md` |
| Architecture & data flow, tech stack decision records | ✅ | `docs/01`, `docs/02` |
| RT contract, primitives (SVF, biquad, LR4, oversampler, TP detector, FFT, SPSC, delay) | ✅ | `core/include/flub/**`, `tests/test_primitives.cpp` |
| All processing modules + meters | ✅ | `core/src/dsp`, `core/src/analysis`, one test file per module |
| Engine: parameters + A/B, macros, governor, auto-level, chain, mixer, presets | ✅ | `core/src/engine`, `tests/test_engine.cpp` |
| JUCE app shell, engine host, GUI; plug-in; batch CLI | ✅ | `app/`, `plugin/`, `tools/` |
| Platform integration scaffolding (Win/mac/Linux) | ✅ | `app/Source/platform`, `platform/` |
| CI (Linux/Windows/macOS, sanitizers, app builds) | ✅ | `.github/workflows/ci.yml` |

---

## 4. Phase 1 — MVP (weeks 4–13, Windows)

**Goal:** a stable Windows app that people use every day for music and games. It runs on a single strip fed by a third-party virtual cable (VB-Cable) or any capture device, and outputs to a WASAPI device.

| # | Work item | Estimate (person-weeks) | Notes |
|---|---|---|---|
| 1.1 | Engine host hardening: device selection, reconnect on hot-plug/sleep, drift-compensated input FIFO soak | 3 | `AudioEngineHost`, `DriftCompensatedFifo` |
| 1.2 | Latency measurement tool (loopback impulse) + latency HUD | 1.5 | Validates the per-profile budgets on real devices |
| 1.3 | Preset browser UX, user presets, A/B banks, loudness-matched bypass UX | 2 | |
| 1.4 | DSP tuning pass with listening panel (8 listeners, 40 reference tracks, 6 games) | 3 | Macro curves, preset voicing |
| 1.5 | Tray, hotkeys, start-with-Windows, close-to-tray | 1 | |
| 1.6 | Onboarding: device check, "disable OEM enhancements" guide, headphones vs speakers | 1.5 | |
| 1.7 | Installer (WiX MSI, no driver yet), code signing, auto-update check | 2 | |
| 1.8 | Crash reporting (Crashpad, opt-in), logging (never on the audio thread) | 1 | |
| 1.9 | Performance: profiling, SIMD for hot loops (EQ bands, TP detector), PFFFT in the analyser | 2 | |
| 1.10 | QA: device matrix (20 devices: USB DACs, onboard Realtek, BT, HDMI, plus every Turtle Beach family on each of its connection types; see `docs/10-headset-compatibility.md` §5), 8 h soak, glitch detector | 4 | Flip `labVerified` per device profile as models pass |

**Exit criteria**
- 8-hour soak at 48 kHz / 128 frames with **zero** dropouts on three reference machines (desktop, gaming laptop, low-end laptop).
- CPU under 3 % of one core on a mid-range laptop in the Balanced profile with all modules on.
- Measured added latency under 10 ms (Balanced, shared low-latency WASAPI).
- No true-peak overs across the full test corpus (automated).
- Closed beta: ≥ 70 % of 200 users rate the sound "better than off" in loudness-matched blind A/B.

---

## 5. Phase 2 — Advanced (weeks 14–29, Windows)

**Goal:** remove the third-party cable dependency, add per-application profiles and routing, deliver the full gaming feature set, and ship the plug-in.

| # | Work item | Estimate (pw) | Notes |
|---|---|---|---|
| 2.1 | **Flubsound Virtual Audio** WaveRT driver: 4 render endpoints (Game 7.1, Music, Chat, System) + Mic | 8 | Based on SYSVAD / SimpleAudioSample (MS-PL) |
| 2.2 | Shared-memory zero-copy path (private IOCTL) + clock slaving | 4 | `platform/windows/driver/FlubVirtualAudioShared.h` |
| 2.3 | Driver signing: EV certificate, attestation signing, HVCI validation, installer integration | 3 | Start the paperwork in week 10 |
| 2.4 | Multi-strip engine UI: routing panel, per-strip profiles, per-app assignment (policy API adapter + fallback) | 4 | `MixEngine`, `AppAudioRouter` |
| 2.5 | Auto-profile: foreground game detection → profile switch (allow-list of game executables) | 1.5 | |
| 2.6 | Virtualiser HQ: SOFA loader, resampling of HRIRs, uniformly partitioned FFT convolution | 3 | Parametric model stays the default |
| 2.7 | Gaming tuning: footstep/anti-masking bands, per-genre presets, competitive latency validation | 3 | Playtests with 20 competitive players |
| 2.8 | Spectral gate UX (learn/freeze noise profile), dynamic EQ editor with live gain display | 2 | |
| 2.9 | Plug-in (VST3/AU) with the custom editor shared with the app, pluginval in CI | 2 | |
| 2.10 | Batch processing UI in the app (drag-and-drop folder, loudness target, export formats) | 2 | Uses the same engine as `flubsound-cli` |
| 2.11 | Hearing guard, loudness history export | 1 | |
| 2.12 | QA: driver stress (sleep/resume, device churn, 24 h soak), anti-cheat compatibility matrix (EAC, BattlEye, Vanguard, VAC) | 4 | |

**Exit criteria**
- The driver installs cleanly with Secure Boot, HVCI and Smart App Control on Windows 10 22H2 and Windows 11 23H2+.
- 24 h soak with 4 strips active and zero glitches.
- Per-app routing works on the supported Windows builds, with a graceful fallback elsewhere.
- No anti-cheat conflicts across the top 20 competitive titles.
- Public beta NPS ≥ 40.

---

## 6. Phase 3 — Polish (weeks 30–41, macOS + Linux + 1.0)

| # | Work item | Estimate (pw) | Notes |
|---|---|---|---|
| 3.1 | macOS virtual device (Audio Server Plug-in via libASPL) | 4 | |
| 3.2 | macOS per-app capture with Core Audio process taps (14.2+), permissions UX | 3 | |
| 3.3 | macOS packaging: universal binary, hardened runtime, notarised `.pkg` | 1.5 | |
| 3.4 | Linux: PipeWire null-sink setup in-app, WirePlumber rules, packages (`.deb`/`.rpm`, Flatpak) | 3 | |
| 3.5 | Linux: native `pw_filter` node prototype (lowest latency) | 2 | Can slip to 1.1 |
| 3.6 | UI polish: motion, onboarding wizard, accessibility audit (screen readers, keyboard, colour-blind palette), high-DPI | 4 | |
| 3.7 | Localisation (EN, DE, FR, ES, PT-BR, JA, KO, ZH) | 2 | |
| 3.8 | Headphone correction profiles (target curves) as EQ presets | 2 | Check dataset licences |
| 3.9 | Performance and power: battery impact on laptops, idle detection (bypass DSP on digital silence) | 2 | |
| 3.10 | Documentation, website, support knowledge base | 2 | |
| 3.11 | Release engineering: update channel, rollback, telemetry (opt-in) | 2 | |

**1.0 exit criteria**
- Zero P1 bugs.
- Crash-free sessions ≥ 99.8 %.
- The Phase 1 and 2 gates re-run green on all platforms.
- Notarisation and signing are automated in CI.
- Documentation is complete.

---

## 7. Test strategy per phase

| Level | What | When |
|---|---|---|
| Unit (property-based) | Frequency responses vs analytic curves, latency exactness, mono-sum invariance, true-peak ceiling, zero allocations, block-size invariance, finite outputs | Every commit (CI) |
| Sanitizers | ASan + UBSan full suite; RTSan on `[[clang::nonblocking]]` entry points | Every commit (CI) |
| Integration | Chain/mixer tests, preset validation (each factory preset rendered and checked), CLI end-to-end | Every commit (CI) |
| Golden renders | Reference programme through every preset; spectral/loudness diffs with tolerances | Nightly |
| Soak | 8–24 h device runs with a glitch detector (sine through a loopback cable, discontinuity detection) | Weekly + release candidates |
| Listening tests | Loudness-matched blind A/B (MUSHRA-style for music; task-based localisation tests for gaming) | Each tuning milestone |
| Compatibility | Device lab, Windows builds, anti-cheat matrix, macOS versions, distros | Each beta |

---

## 8. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Driver signing / attestation delays | Medium | High | Start the EV certificate and Partner Center enrolment in Phase 1. Ship Phase 1 without a driver (virtual cable + process loopback). |
| Undocumented per-app routing API changes | High | Medium | Isolated adapter, per-build validation, documented fallback UI, process-loopback path. |
| Anti-cheat false positives | Low | High | No injection or hooks; signed driver; early outreach to vendors; compatibility matrix in QA. |
| Tuning "too loud / fatiguing" backlash | Medium | Medium | Conservative defaults, governor, loudness-matched A/B, hearing guard, listening panel. |
| macOS process-tap API gaps or regressions | Medium | Medium | Virtual device fallback with manual routing; target 14.2+ only for per-app. |
| CPU on low-end laptops | Medium | Medium | Low Latency profile is also the cheapest; per-module CPU budget; idle bypass on digital silence. |
| JUCE licensing cost / terms | Low | Medium | Core is JUCE-free; commercial JUCE licence budgeted. |
| Team availability (kernel expertise is scarce) | Medium | High | Contract a Windows audio-driver specialist for Phase 2 driver work and reviews. |

---

## 9. Definition of done (every feature)

1. Implemented against a written contract (header), with a unit test for every property claimed in its documentation.
2. Real-time safe: allocation-free test and a sanitizer run are both green.
3. Latency and CPU cost measured and recorded in `docs/03-dsp-design.md`.
4. Click-free parameter and bypass behaviour verified.
5. Documented: user-facing text and developer docs.
6. Reviewed by a second engineer (adversarial review checklist in `CONTRIBUTING.md`).
