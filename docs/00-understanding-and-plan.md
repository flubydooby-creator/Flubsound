# 00 — Understanding & Delivery Plan (Workflow 1)

**Project:** Flubsound Pro – Music & Gaming Edition
**Mission:** a production-grade, system-wide, per-application, ultra-low-latency audio enhancer for music and video games. It should make audio louder, punchier, clearer, wider and more immersive while holding distortion, clipping and latency under strict control.

This document confirms the scope, records the interpretation decisions that shape everything downstream, and lays out how the work was delivered. The other documents in `docs/` are the nine mandatory deliverables. The code in this repository is the implementation foundation they describe.

---

## 1. Scope confirmation — requirement checklist

Each requirement has an ID. The traceability matrix in §5 maps every ID to its design section, code and tests.

### R1 Real-time audio engine
| ID | Requirement | Interpretation used |
|---|---|---|
| R1.1 | Latency under 10–12 ms | This is the **added** latency Flubsound introduces on top of what the OS and application already have. We budget ≤ 2 ms algorithmic latency in *Low Latency*, ≤ 4 ms in *Balanced*, plus I/O buffering, for a total added ≤ 10 ms. The *Quality* profile, for music and batch, is allowed more. See `01-architecture.md` §5. |
| R1.2 | WASAPI shared + exclusive, ASIO, Core Audio, ALSA / PulseAudio / PipeWire | JUCE 9 device layer (WASAPI shared, exclusive and low-latency; ASIO with the Steinberg SDK; CoreAudio; ALSA; JACK). PipeWire is reached via its ALSA/JACK/Pulse compatibility layers, and natively later (roadmap). |
| R1.3 | 32-bit float internal processing | All DSP is `float`. Metering integrators and long-running accumulators use `double` where precision matters. |
| R1.4 | 44.1 / 48 / 96 / 192 kHz | Every coefficient is derived from the session rate. Tests cover all four rates. |
| R1.5 | Robust buffer management, glitch-free | Lock-free SPSC FIFOs, no allocation or locks on the audio thread (enforced by tests), drift-compensated resampling between clock domains, and denormal protection. |

### R2 Core processing modules (all real-time, individually bypassable, with A/B)
| ID | Module |
|---|---|
| R2.1 | Intelligent true-peak loudness maximizer + soft clipper |
| R2.2 | Multi-band dynamic EQ + fully parametric EQ (≥ 10 bands) |
| R2.3 | Advanced bass boost + harmonic (psychoacoustic) enhancement |
| R2.4 | Clarity & transient detail enhancer |
| R2.5 | Stereo widener + spatializer (gaming imaging, mono compatible) |
| R2.6 | Look-ahead compressor / limiter |
| R2.7 | Spectral noise gate / light noise reduction |
| R2.8 | Music Mode (punch, width, clarity, loudness, warmth) |
| R2.9 | Gaming Mode (footsteps, positional audio, explosions, gunshots, environmental detail, score clarity) |
| R2.10 | Every module bypassable (click-free, latency-constant) + A/B comparison (two parameter banks, loudness-matched bypass) |

### R3 Boosting & protection
| ID | Requirement |
|---|---|
| R3.1 | Safe gain staging with true-peak limiting |
| R3.2 | LUFS-based auto-gain / loudness matching |
| R3.3 | Saturation: tape, tube, digital |
| R3.4 | Global Boost Intensity 0–100 % that intelligently scales multiple modules |
| R3.5 | Strong anti-clipping + THD protection |

### R4 User experience
| ID | Requirement |
|---|---|
| R4.1 | Modern dark professional GUI |
| R4.2 | Real-time spectrum analyzer, waveform, LUFS / true-peak / RMS meters |
| R4.3 | Rich Music and Gaming preset system |
| R4.4 | System tray + global hotkeys |
| R4.5 | Per-application profiles and routing |
| R4.6 | Virtual audio device / cable support |

### R5 Additional capabilities
| ID | Requirement |
|---|---|
| R5.1 | Batch processing of music files |
| R5.2 | Export of enhanced audio |
| R5.3 | Architecture ready for neural enhancement |
| R6.1 | *(added on request)* Compatible with all Turtle Beach headsets (every connection type on a computer), with device-aware safety and setup advice |

### Technical preferences
C++ with JUCE. The GUI is JUCE, with Electron/React evaluated and rejected, reasons in `02-tech-stack.md`. Platform order is Windows → macOS → Linux. The design must be modular, thread-safe and high-performance.

---

## 2. Key interpretation decisions

These were fixed up front because every later deliverable depends on them. `02-tech-stack.md` and `01-architecture.md` justify them in full.

1. **Framework-independent DSP core.** `flub_core` is pure C++20 with zero dependencies: no JUCE, no allocation or locks on the audio path. The same code runs in:
   - the desktop app,
   - the VST3/AU plug-in,
   - the batch CLI,
   - the unit tests,
   - later, a Windows APO or a PipeWire filter node.

   JUCE is used where it excels: device I/O, GUI and plug-in wrappers.
2. **System-wide + per-app = virtual endpoints + routing.** Flubsound installs a virtual audio driver exposing one endpoint per *strip*: Game (7.1), Music, Chat and System. Applications are routed to endpoints per app. The engine processes each strip with its own profile, sums them, protects the sum with a master true-peak limiter and plays the result on the real device. This is the model proven by SteelSeries Sonar and Voicemeeter.
   - macOS 14.2+ uses Core Audio process taps instead.
   - Linux uses PipeWire null sinks.
   - Where no driver is installed, process-loopback capture or any third-party virtual cable feeds the strips.
3. **Latency is a first-class, constant quantity.** Each latency profile (Quality, Balanced, Low Latency) fixes every structural choice: look-ahead lengths, oversampling factors, and whether the STFT gate is in the chain. Within a profile the chain latency never changes, because bypass paths are latency-compensated. Toggling a module therefore never shifts audio in time or clicks.
4. **Macros, not presets, drive "intelligence".** Presets store *base* values. Boost Intensity and the five mode macros add staged, curved contributions on top. A **Safety Governor** scales back every loudness-adding contribution when limiter gain reduction or clipper energy exceed their budgets, which is the THD protection loop.
5. **Gaming correctness beats loudness.** Gaming mode enforces rules that protect positional cues:
   - all dynamics are stereo-linked,
   - no crossfeed,
   - no widening of binaural (virtualised) output,
   - fast recovery after loud events,
   - dedicated footstep and anti-masking dynamic-EQ bands,
   - a sub-2 ms Low Latency profile.

---

## 3. Delivery plan (the six workflows)

| Workflow | Output | Where |
|---|---|---|
| 1 Understanding & Planning | This document | `docs/00-understanding-and-plan.md` |
| 2 Architecture & Design | Architecture + data flow; tech stack | `docs/01-architecture.md`, `docs/02-tech-stack.md`; contracts in `core/include/flub/**` |
| 3 Deep DSP Design | Module-by-module algorithms, parameters, maths | `docs/03-dsp-design.md` |
| 4 Implementation Foundations | Folder structure; code skeletons (working implementations) | `docs/04-project-structure.md`, `docs/05-code-skeletons.md`; `core/`, `app/`, `plugin/`, `tools/`, `platform/`, `presets/`, `tests/` |
| 5 Interface & Roadmap | GUI structure + components; phased roadmap | `docs/06-gui.md`, `docs/07-roadmap.md`; `app/Source/ui/**` |
| 6 Risk & Future | Pitfalls + concrete solutions; expansion roadmap | `docs/08-pitfalls-and-solutions.md`, `docs/09-future-roadmap.md` |

### Execution method

The work was run as a sequence of review loops:

1. **Contracts first.** The shared primitives were written and tested by hand: SVF, biquad, Linkwitz–Riley crossovers, half-band oversampler, true-peak interpolator, FFT, lock-free ring, delay line. Then every module's public header was written as a contract, with the algorithm specified in its comments.
2. **Parallel implementation with adversarial verification.** Each module was implemented against its contract with thorough tests. An independent reviewer then tried to break it and fixed the real defects found. Covered in this way: EQ, dynamic EQ, bass, clarity, saturation, spatializer, virtualizer, compressor, limiter/maximizer, noise gate, loudness meter and WAV/JSON I/O.
3. **Engine integration.** Parameter layout, macros, protection loops, module slots, processing chain and multi-strip mixer, verified by chain-level tests: latency constancy, bypass transparency, ceiling safety, zero allocations and 7.1 folding.
4. **Application, platform, tools, presets.** The JUCE app, the Windows/macOS/Linux integration layer, the CLI batch processor, the plug-in and the factory presets, each with a review pass.
5. **Documentation.** Written from the actual code and fact-checked against it.
6. **Completeness loop.** A final critic checks every requirement ID in §1 against the matrix below. Gaps go back into the loop until none remain.

### Quality gates (applied to every change)

- Zero-warning builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` on GCC and Clang, and `/W4` on MSVC.
- The full unit-test suite passes under Address and Undefined Behavior sanitizers.
- Allocation-counting tests prove `process()` paths never touch the heap.
- CI on Windows, macOS and Linux (`.github/workflows/ci.yml`).

---

## 4. Deliverable index

1. [High-level architecture & data flow](01-architecture.md)
2. [Tech stack & justification](02-tech-stack.md)
3. [DSP design, module by module](03-dsp-design.md)
4. [Project / folder structure](04-project-structure.md)
5. [Core DSP code skeletons (guided tour of the real code)](05-code-skeletons.md)
6. [GUI structure & components](06-gui.md)
7. [Implementation roadmap: MVP → Advanced → Polish](07-roadmap.md)
8. [Major pitfalls & concrete solutions](08-pitfalls-and-solutions.md)
9. [Future expansion roadmap](09-future-roadmap.md)
10. [Headset compatibility, including all Turtle Beach headsets](10-headset-compatibility.md) (added on request)

---

## 5. Requirement traceability matrix

See [`TRACEABILITY.md`](TRACEABILITY.md). It is generated at the end of the delivery loop and lists, for every requirement ID, the design section, the implementing files and the tests that prove it.
