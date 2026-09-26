# 08 — Major Pitfalls & Concrete Solutions (Workflow 6)

Each pitfall below lists:
- **Symptom**: what users or testers experience.
- **Root cause**.
- **Solution**: what Flubsound does, pointing at the code or design that implements it.
- **Verification**: how we prove it stays fixed.

Pitfalls are grouped by the four areas the brief singles out: latency, clipping/distortion, gaming sensitivity and platform issues. Engineering-process, product and safety pitfalls follow.

---

## A. Latency

### A1. Buffers stack up silently
- **Symptom:** "It adds 40 ms and ruins my aim." Each layer looks small, but together they are not.
- **Root cause:** added latency is the sum of the virtual endpoint period, capture safety margin, processing block, algorithmic look-ahead, output device buffer and any sample-rate converters.
- **Solution:**
  1. An explicit **latency budget per profile** (documented in `01-architecture.md` §5): *Low Latency* ≈ 2.1 ms algorithmic, *Balanced* 4.0 ms, *Quality* (≈ 28 ms) for music/batch only; the desktop app's master limiter adds 0.9 ms in *Low Latency* (0.5 ms look-ahead when every strip runs that profile) and 1.4 ms otherwise.
  2. **Event-driven** WASAPI with small periods: `IAudioClient3` low-latency shared mode, or exclusive mode. Low-latency shared mode is the default when no device choice is saved: the app opens JUCE's *Windows Audio (Low Latency Mode)* type, falling back to *Windows Audio* if the device cannot be opened in that mode. A type picked in Settings > Audio is saved and wins.
  3. The **driver shared-memory path** (design), which reads the virtual endpoint's cyclic buffer directly instead of paying for a second WASAPI capture hop (`platform/windows/driver/`).
  4. A 128-frame block at 48 kHz.
  5. The latency shown in the UI is the *reported* total: device input + device output latency (as the driver reports them) + engine latency + the capture FIFO target. It is not measured.
- **Verification:** `tests/test_engine.cpp` "Chain: latency per profile and constant under module bypass" asserts Balanced < 5 ms and Low Latency < 2.5 ms algorithmic latency. A loopback-impulse measurement tool (roadmap Phase 1, item 1.2) will measure real end-to-end latency per device.

### A2. Look-ahead and linear-phase creep
- **Symptom:** latency grows release after release.
- **Root cause:** every module "just needs 1 ms of look-ahead" or a linear-phase filter.
- **Solution:**
  - Only two modules may have look-ahead: compressor and limiter.
  - Nonlinear stages use half-band FIR oversampling with **integer, documented** latency (16, 32 or 36 samples in the profiles).
  - All EQ is minimum-phase IIR (TPT SVF). There is no linear-phase EQ; if one is added it belongs to offline/batch use only.
  - The STFT noise gate is excluded from the chain outside the *Quality* profile.
- **Verification:** each module's `latencySamples()` is unit-tested against a delayed impulse. The chain test enforces the per-profile totals.

### A3. Latency that changes at runtime
- **Symptom:** clicks and echoes when toggling modules, and slowly drifting A/V sync.
- **Root cause:** bypassing a module with latency removes its delay; changing look-ahead mid-stream shifts audio in time.
- **Solution:**
  - `ModuleSlot` keeps the dry path delayed by the module's latency and crossfades over 20 ms. It pre-rolls re-enabled modules so look-ahead lines are primed.
  - The global bypass uses a dry path delayed by the full chain latency.
  - Toggling the virtualiser on a 5.1/7.1 strip crossfades the binaural render and the BS.775 downmix over 20 ms ("Chain: toggling the virtualiser on a 7.1 strip crossfades (no step in the output)").
  - Latency-affecting settings are **structural**, changeable only through the latency profile, which triggers an explicit re-prepare (`ProcessingChain::needsReprepare()`).
- **Verification:** "ModuleSlot: bypassed slot is a pure latency-compensated delay and toggling is click-free" (error ≤ 1e-5) and "Chain: everything bypassed = input delayed by the chain latency (bit-transparent path)" (error ≤ 1e-6).

### A4. Lip-sync for video
- **Symptom:** dialogue feels late in films and cut-scenes.
- **Solution:** keep added latency far below perceptual thresholds. ITU-R BT.1359 puts detectability at roughly 45 ms audio-early and 125 ms audio-late. *Balanced* (~4 ms algorithmic + I/O) is well inside that, and even *Quality* (~28 ms algorithmic) is acceptable for video.

### A5. Bluetooth headphones
- **Symptom:** "Flubsound adds 200 ms." In fact, the Bluetooth codec does.
- **Root cause:** SBC/AAC/aptX pipelines add 100–300 ms. Lossy codecs also reconstruct peaks *above* the input true peak.
- **Solution:**
  - Detect Bluetooth endpoints (platform transport query on Windows / macOS, name and format heuristics otherwise). The device advice banner states that Bluetooth adds its own ~100–300 ms and suggests the *Bluetooth Headphones* device preset. (A separate codec-latency figure in the latency readout is not implemented.)
  - The master limiter's ceiling is capped at **−2 dBTP** on A2DP and **−3 dBTP** in hands-free mode, and the *Bluetooth Headphones* preset itself uses −2 dBTP, to leave room for codec overshoot.

### A6. Hidden resampling
- **Symptom:** extra latency plus a slight high-frequency haze.
- **Root cause:** in shared mode the Windows audio engine resamples to the endpoint mix format; if the virtual endpoint, engine and physical device disagree, audio is converted twice.
- **Solution (design):** default every Flubsound endpoint and the engine to the physical device's mix rate (usually 48 kHz), with the virtual driver advertising the same format (float32 / 48 kHz default, `platform/windows/driver/`). Flagging a rate mismatch in the UI is roadmap; today the engine simply runs at the rate the output device is opened with.

---

## B. Clipping, distortion & gain staging

### B1. Inter-sample peaks
- **Symptom:** a "0 dBFS"-safe master still distorts in DACs, lossy encoders and Bluetooth.
- **Root cause:** the reconstructed analog waveform exceeds the sample peaks by up to ~3 dB for high-frequency content.
- **Solution:** `TruePeakLimiter` detects on a 4× polyphase-interpolated signal, the 4× oversampling approach of ITU-R BS.1770-4 Annex 2, using the meters' own interpolator (`TruePeakDetector`: 40 taps per phase, Kaiser β 5, flat within −0.02 / +0.04 dB up to 0.4535 fs), plus parabolic refinement of each local maximum. The required gain is computed against a threshold 0.05 dB below the ceiling. A sliding-minimum + box-filter gain envelope provably reaches that gain before the peak leaves the look-ahead, and in true-peak mode holds it flat for Kh = min(8, L/3) samples on either side of the peak, so the gain does not move under the central taps of the interpolator that reads it. The default ceiling is −1 dBTP (device profiles cap it at −2 on Bluetooth, −3 in hands-free mode), with a final counted safety clamp.
- **Verification:** limiter tests drive band-limited noise (+6 / +20 dB), band-limited squares, sparse impulses and 997 Hz / 11 kHz sines at 44.1/48/96 kHz, at ceilings of −1 and −0.1 dBTP. They check sample peak ≤ ceiling and an *independent* true peak (ideal sinc reconstruction via an 8× zero-padded FFT) ≤ ceiling + 0.1 dB, with `getSafetyClipCount() == 0`. Raw full-band signals (white noise, aliased squares) get the exact sample-peak guarantee, and their ideal-reconstruction peak is bounded loosely (≤ +2 dB); every 4× interpolator rolls off near fs/2.

### B2. Boost stacking
- **Symptom:** EQ + bass + exciter + saturation + macros add up to 20 dB and everything slams the limiter.
- **Solution (gain staging by construction):**
  1. Saturation, harmonics and exciters have **unity small-signal gain** (`f(gx)/g`), so they change timbre, not level. (The saturator has no automatic make-up, so loud material comes out *quieter* at high drive: pink noise at −12 dBFS RMS through Tape at 9 dB drive loses about 3.6 dB RMS.)
  2. Bass boost is **headroom-protected**: it is withdrawn when the predicted low-frequency level would exceed `bass.protect`.
  3. **AutoLevel** normalises the input loudness.
  4. The **SafetyGovernor** scales every loudness-adding macro contribution back when average limiter gain reduction is below −6 dB or clipper energy is above −30 dB. It recovers slowly, with hysteresis.
  5. The limiter is always last, and the mixer has a master true-peak limiter after strip summing.
- **Verification:** "Chain: full Music boost on a hot programme never exceeds the ceiling" (every macro at 100 %, hot input, both modes) and the governor unit test.

### B3. Aliasing from nonlinear stages
- **Symptom:** gritty, inharmonic "digital" distortion, most obvious on cymbals and at 44.1 kHz.
- **Solution:**
  - Clipper: 4× oversampling (2× in Low Latency).
  - Saturation: 2× oversampling.
  - The air exciter band-limits its input to 3.5–7 kHz (24 dB/oct skirts) and uses polynomial order ≤ 3, so products of in-band content stay below 21 kHz without oversampling. Strong tones on the upper skirt (7.6–9 kHz) still leave small aliases (−25 to −44 dB at 44.1 kHz), and the chain disables air below 42 kHz sample rate.
  - The psychoacoustic bass waveshaper operates on content below ~250 Hz, so its harmonics are nowhere near Nyquist.
  - Antiderivative anti-aliasing (ADAA) is on the roadmap for a zero-latency clipper option.
- **Verification:** "Saturator: 4x HQ Digital keeps 15 kHz aliasing below -60 dB (1x does not)" (the chain runs the saturator at 2×, where a 15 kHz torture tone still aliases at roughly −19 dB with Tape and −32 dB with Digital, see `03-dsp-design.md`), "Clarity: air produces no aliasing at 44.1 kHz", and "Transparency: the maximizer's oversampled clipper does not droop the top octave".

### B4. Limiter pumping and "flattening"
- **Symptom:** music breathes; after a gunshot everything ducks for half a second.
- **Solution:**
  1. A soft clipper handles sub-millisecond peaks, so the limiter sees fewer, longer overs.
  2. *Glue*: 3-band pre-compression (120 Hz / 4 kHz, 2:1 above ceiling − 6 dB) with phase-coherent LR4 splitting, so bass does not drive broadband gain reduction. The splitter is only in the path while glue is armed (preset `max.glue` > 0, or Boost / Loudness above zero in Music mode).
  3. **Program-dependent release**: fast (release/5) for isolated peaks, blended to the full release as a limiting run spans 25 → 50 ms (overs less than 25 ms apart count as one run).
  4. The governor caps sustained gain reduction.

### B5. DC and sub-sonic energy
- **Symptom:** the limiter reacts to nothing audible, and woofer excursion rises.
- **Root cause:** asymmetric (tube) saturation creates DC, and sub-20 Hz rumble eats headroom.
- **Solution:** 10 Hz DC blocker after tube saturation, and a 24 dB/oct subsonic filter (default 20 Hz) at the head of the bass engine.

### B6. NaN/Inf poisoning
- **Symptom:** permanent silence or a burst of noise after one bad buffer from a driver or plug-in.
- **Root cause:** a single NaN latches forever in IIR state.
- **Solution:** every module is tested with silence, DC, full-scale noise, impulses and extreme parameters for finite output. The chain input is also guarded: a non-finite block is zeroed and the chain reset, instead of propagating.

### B6b. Real-time levelling during pauses and fade-outs
- **Symptom:** the level jumps up after a pause, or a fade-out gets "pulled back up".
- **Root cause:** a naive running loudness estimate keeps decaying through silence, and the levelling loop chases it.
- **Solution:** all loudness control loops (AutoLevel, AutoDrive, LoudnessMatch) use `GatedLoudness`. The slow 3 s measure only advances while programme is present: block RMS above −70 dBFS, a fast 100 ms follower above −50 LUFS, and within 20 LU of the slow value. Adaptation is additionally slew-limited to +1 dB/s up and −4 dB/s down.
- **Known limit:** a slow musical fade can still lift the gain by a dB or two before the relative gate closes. This is inherent to any look-ahead-free leveller. Players that support per-track loudness normalisation (ReplayGain / LUFS) should use it, and AutoLevel stays off in the gaming presets.
- **Verification:** "AutoLevel: brings a quiet source towards the target, slew limited, frozen in silence" (`tests/test_engine.cpp`).

### B7. Denormals
- **Symptom:** CPU spikes during fade-outs and silence, 10–100× slower on x86.
- **Solution:** `ScopedNoDenormals` (FTZ + DAZ on SSE, FZ on AArch64) in every realtime entry point: device callback, plug-in `processBlock` and batch render.

---

## C. Gaming sensitivity

### C1. Unlinked dynamics move sounds
- **Symptom:** enemies seem to be in the wrong place when it gets loud.
- **Root cause:** per-channel gain reduction changes interaural level differences.
- **Solution:** every dynamics processor detects on the max across all channels and applies identical gain: compressor, limiter, dynamic EQ, transient shaper and bass protection. Each has a linking test, e.g. "DynamicEq: detection is stereo linked (a loud L drives the gain of a quiet R)" and "TruePeakLimiter: detection is linked - a quiet channel gets the loud channel's gain".

### C2. Compression destroys distance cues
- **Symptom:** far footsteps sound as close as near ones.
- **Root cause:** loudness is a primary distance cue, and heavy compression flattens it.
- **Solution:**
  - Instead of global squashing, Gaming uses **targeted** processing: upward compression of *quiet high-frequency detail* only (footstep band 3.2 kHz, `BoostBelow`) and **anti-masking** of very loud lows (explosions).
  - The upward compressor has a noise-floor taper and a gentle ratio.
  - The *Competitive FPS* preset keeps broadband *downward* compression off (ratio 1:1), so the compressor only lifts quiet sounds.

### C3. Wideners and crossfeed corrupt localisation
- **Symptom:** blurry or phasey positions, and "inside-the-head" confusion.
- **Solution:**
  - Width is applied to the side signal only, and only above `spatial.lowCut` (default 180 Hz; the mono sum is preserved exactly).
  - Gaming mode forces crossfeed to 0.
  - When the virtualiser produced binaural output, width, space and crossfeed are **locked off** (binaural lock). Only the ILD-emphasising *positional focus* remains.

### C4. Double HRTF
- **Symptom:** muffled, comb-filtered, "far away" audio.
- **Root cause:** the game (or Windows Sonic / Dolby Atmos for Headphones) already renders binaural, and Flubsound virtualises again.
- **Solution:** the virtualiser only runs on 5.1/7.1 input, never on stereo. The rule is *either* the game's HRTF with a stereo endpoint, *or* the game in 7.1 plus Flubsound's virtualiser, never both. The *7.1 Headphone Surround* preset description and the headset device-profile advice say so; an onboarding page that explains it is roadmap (`07-roadmap.md` item 1.6).

### C5. Masking after loud events
- **Symptom:** footsteps inaudible for a second after a grenade.
- **Solution:** a fast auto-release limiter (release/5 for isolated peaks), the anti-masking low-shelf dynamic-EQ band (gaming mode band 6, 90 Hz, scaled by *Footsteps*), per-band glue where a preset arms it (e.g. *Cinematic Adventure*; no Gaming macro raises glue), and a fast bass-protection release (150 ms).

### C6. Front/back confusion in virtual surround
- **Symptom:** rear sounds are heard in front.
- **Root cause:** a spherical head is front/back symmetric.
- **Solution:** a rear pinna-shadow high shelf (−4 dB at 4 kHz for |azimuth| > 90°), head-radius personalisation (`virt.headRadius`, 70–105 mm), early reflections for externalisation (`virt.room`), and measured HRIRs as the HQ renderer. The core already has a direct-form HRIR renderer (`HeadphoneVirtualizer::setHrirSet`, capped at 1024 taps); a SOFA loader, HRIR resampling and partitioned FFT convolution are on the roadmap.

### C7. Anti-cheat and tournament rules
- **Symptom:** the game refuses to start, or the player is flagged.
- **Solution:**
  - Flubsound **never injects into or hooks game processes**. It only receives audio the OS delivers to its virtual endpoints.
  - The planned driver is to be properly signed (attestation → WHQL) and HVCI-compatible (`platform/windows/driver/README.md` §8).
  - Some tournaments restrict third-party audio processing, so the *Tournament Clean* preset limits processing to ceiling protection, and a single hotkey (default Ctrl+Alt+F, Windows and macOS) toggles full bypass on every strip.

### C8. Voice chat problems
- **Symptom:** your team hears echo, or your voice pumps.
- **Solution:** chat is a separate strip with its own profile, and the mic is not routed through music/game processing (Flubsound does not process the microphone in this version). With the virtual driver (design), the user makes the Chat endpoint the Windows default *communications* device so OS ducking applies only there; Flubsound never changes default devices itself.

### C9. Headset on-board DSP stacking with Flubsound (e.g. Turtle Beach Superhuman Hearing)
- **Symptom:** harsh, hissy footsteps; boomy bass; a muffled "far away" surround image.
- **Root cause:** many gaming headsets and their companion apps apply their own footstep enhancement, bass boost, EQ or virtual surround. Stacked with Flubsound's Footsteps / Impact macros or virtualiser, the same processing runs twice.
- **Solution:** device profiles (`presets/devices/device-profiles.json`, `core/include/flub/engine/DeviceProfiles.h`) recognise the headset family and show targeted advice: neutral headset mode, one footstep enhancer, one HRTF stage. They also cap the ceiling per connection (Bluetooth −2 dBTP, hands-free −3 dBTP). The full Turtle Beach matrix is in `docs/10-headset-compatibility.md`.
- **Verification:** `tests/test_device_profiles.cpp` (family matching, no false positives, connection detection, advice and ceiling caps, embedded database identical to the JSON) and "Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz)".

---

## D. Platform issues

### D1. Clock drift between clock domains (Windows / macOS / Linux)
- **Symptom:** a click every few minutes, or slowly growing latency.
- **Root cause:** the virtual endpoint (system timer) and the DAC crystal run at slightly different rates (±100 ppm is common).
- **Solution:** `DriftCompensatedFifo` (`app/Source/engine/`), an SPSC FIFO feeding an adaptive cubic Hermite resampler whose ratio is steered by a PI controller on the FIFO fill level (target: 2 device blocks, raised to the largest recent capture burst + 1 block; critically damped at 0.15 rad/s; correction clamped to ±0.5 %). It serves the per-app capture streams. The driver design also allows *clock slaving*, where engine pulls advance the virtual endpoint position, removing drift at the source.
- **Verification:** not automated yet. Planned: a unit test with simulated ±200 ppm drift over long runs (no underruns or overruns, bounded fill level) and the Phase 1 soak test.

### D2. Exclusive mode locks everyone else out (Windows)
- **Solution (partly roadmap):** default to shared low-latency mode (`IAudioClient3`, JUCE's *Windows Audio (Low Latency Mode)*). Without a saved device choice the app opens that type, falling back to *Windows Audio* if the device cannot be opened in it; a mode chosen in Settings > Audio is saved and kept. Offering exclusive mode only when the virtual endpoints are the system default (so all apps flow through Flubsound) is roadmap.

### D3. Device changes, hot-plug, sleep/resume
- **Solution:** with the virtual driver (design), keep the virtual endpoint as the Windows default so applications never see the physical device disappear. The engine follows device changes through JUCE's device-change notifications (no separate `IMMNotificationClient`). Settings persist JUCE's device state (by device name) and the *preferred* output name; when the preferred output disappears JUCE falls back to another device, and `EngineController` switches back once the preferred one is listed again (rescan every 5 s while it is missing).

### D4. Double processing by OEM "enhancements"
- **Symptom:** harsh or boomy sound, or effects stacked on effects.
- **Root cause:** OEM APOs (e.g. laptop "audio enhancements"), Windows Spatial Sound or loudness equalisation are active on the physical endpoint.
- **Solution (roadmap, `07-roadmap.md` item 1.6):** detect and warn in onboarding with a direct link to the endpoint's properties, and recommend disabling enhancements on the output device Flubsound feeds. Today only headset-specific advice exists (device profiles, C9).

### D5. Power management (Windows 11 EcoQoS, hybrid CPUs)
- **Symptom:** dropouts when the window is minimised.
- **Root cause:** Windows parks background processes on efficiency cores and throttles timers.
- **Solution:** `SetProcessInformation(ProcessPowerThrottling)` opt-out plus MMCSS "Pro Audio" on the audio thread (`PlatformServices_win.cpp`).

### D6. Driver signing & security features
- **Symptom:** "driver blocked", or the install fails with Secure Boot / HVCI / Smart App Control.
- **Solution:** EV code-signing certificate, Microsoft attestation signing via Partner Center (WHQL later for broad OEM trust), HVCI-compatible driver code (no executable pool allocations, no writable+executable sections), and installer checks with clear messages.

### D7. Undocumented per-app routing API
- **Symptom:** per-app assignment silently stops working after a Windows update.
- **Solution:** the adapter is compiled in only with `FLUB_ENABLE_UNDOCUMENTED_ROUTING` (off by default) and runs only on explicit user action. It requests the factory by the interface id that matches the Windows build (the id changed at build 21390), so an unknown layout fails cleanly with `E_NOINTERFACE`, and every failure falls back to opening `ms-settings:apps-volume` with guidance. It does not read the setting back yet (`GetPersistedDefaultAudioEndpoint` is declared but unused). Process-loopback capture offers a documented monitoring path.

### D8. macOS permissions and installation
- **Symptom:** no audio from process taps, or the virtual device is missing.
- **Solution (design, `platform/macos/README.md`):**
  - Request the audio-capture TCC permission with a clear purpose string (`NSAudioCaptureUsageDescription`). Today the app only declares the microphone permission, which capturing a loopback device needs.
  - The HAL plug-in is installed by a signed, notarised `.pkg` into `/Library/Audio/Plug-Ins/HAL`, followed by a `coreaudiod` restart.
  - Universal binaries (arm64 + x86_64).
  - Process taps require macOS 14.2+; older systems fall back to the virtual device with manual routing.

### D9. Linux diversity
- **Symptom:** high latency or xruns.
- **Solution:** document the PipeWire quantum (`platform/linux/README.md`: `pw-metadata -n settings 0 clock.force-quantum 256`), prefer the JACK / PipeWire-JACK device type (the server then calls the engine on its own real-time thread), and otherwise promote the audio thread to best-effort `SCHED_FIFO` 20 (or the user's `RLIMIT_RTPRIO`). Roadmap: RealtimeKit over D-Bus, setting `node.latency` for Flubsound's own nodes, and portals / libpipewire for Flatpak.

### D10. ASIO specifics
- ASIO drivers are often single-client. When ASIO is the output, Flubsound is that client and every application must route through Flubsound's virtual endpoints. A UI note that says so is roadmap; ASIO itself is only built when `FLUB_ASIO_SDK_DIR` is set.

---

## E. Engineering & real-time software

| Pitfall | Solution in Flubsound |
|---|---|
| Allocation / locks / logging on the audio thread (priority inversion, page faults) | A written RT contract in `Processor.h`. `AllocationGuard` tests over every module, the whole chain and the mixer (0 allocations, including mode and A/B switches). Clang RealtimeSanitizer in CI (job `rtsan`, `FLUB_RTSAN=ON`, Clang 20): `ProcessingChain::process`, `MixEngine::process` and every `Processor::process` override are `[[clang::nonblocking]]` (`FLUB_NONBLOCKING`, `common/Realtime.h`), so any allocation, free, lock or blocking system call reached from them aborts the full test run; `tests/test_rtsan.cpp` checks that the annotations are in place and that the sanitizer really stops a violation. Not covered by RTSan: `reset()`, setters and the app's own callback code. |
| GUI ↔ audio data races | Three main channels: `ParameterStore` (relaxed atomics, two banks), `MeterBus` (atomics) and `AnalyzerTaps` (SPSC rings), plus single atomic hand-offs (effective values, audition mask, strip gain / ceiling). No shared mutable objects and no audio-thread callbacks into the GUI. |
| Object lifetime when swapping engines/HRIR sets | Structural changes happen in `prepare()` off the audio thread. The documented roadmap design is a double-buffered engine swap with deferred (RCU-style) reclamation on the message thread. |
| Zipper noise | All continuous parameters are smoothed. Discrete changes (filter type, bypass) crossfade. Coefficients are updated at a 16-sample control rate using modulation-safe SVFs. |
| Block-size dependence | Every module is tested for identical output with 1, 7, 64 and 512-sample blocks (control counters carry across blocks). |
| Preset breakage between versions | String keys, versioned files, unknown keys ignored, missing keys defaulted, choices stored as labels. |
| Flaky "golden file" audio tests | Property-based assertions instead: response matches the analytic curve, mono sum is preserved, ceiling holds, latency is exact, output is finite. Golden renders are planned only for regression diffs with tolerances (`07-roadmap.md` §7). |

---

## F. Product & safety

| Pitfall | Solution |
|---|---|
| "Louder = better" bias hides bad processing | Loudness-matched global bypass (the dry path is matched to the processed loudness and never pushed above the ceiling), plus A/B banks. |
| Hearing damage from boosted loudness | Output loudness readouts (momentary / short-term / integrated LUFS, true peak) exist, and defaults are conservative (Boost 0 %, ceiling −1 dBTP). Roadmap (`07-roadmap.md` items 2.11 and 1.6): a hearing-guard notice when sustained high short-term loudness is detected, and WHO safe-listening guidance (≈ 80 dB(A) for 40 h/week) cited in onboarding. |
| Users stack Flubsound with other enhancers | Today: device-profile advice for headsets with their own DSP (C9). Roadmap: onboarding checks for other enhancement software, OEM APOs and spatial sound, with guidance to disable them. |
| Too many knobs | A three-level UI: Boost Intensity + 5 mode macros → module cards with key controls → full parameter lists. Presets set base values and macros add staged contributions on top. |
