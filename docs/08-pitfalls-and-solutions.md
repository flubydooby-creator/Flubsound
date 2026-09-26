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
  1. An explicit **latency budget per profile** (documented in `01-architecture.md` §5): *Low Latency* ≈ 2 ms algorithmic, *Balanced* ≈ 4 ms, *Quality* for music/batch only.
  2. **Event-driven** WASAPI with small periods: `IAudioClient3` low-latency shared mode, or exclusive mode.
  3. The **driver shared-memory path**, which reads the virtual endpoint's cyclic buffer directly instead of paying for a second WASAPI capture hop (`platform/windows/driver/`).
  4. A 128-frame block at 48 kHz.
  5. The latency reported in the UI is the *measured* total: device in + device out + engine.
- **Verification:** `tests/test_engine.cpp` "Chain: latency per profile…" asserts Balanced < 5 ms and Low Latency < 2.5 ms algorithmic latency. A loopback-impulse measurement tool (roadmap Phase 2) measures real end-to-end latency per device.

### A2. Look-ahead and linear-phase creep
- **Symptom:** latency grows release after release.
- **Root cause:** every module "just needs 1 ms of look-ahead" or a linear-phase filter.
- **Solution:**
  - Only two modules may have look-ahead: compressor and limiter.
  - Nonlinear stages use half-band FIR oversampling with **integer, documented** latency (16–36 samples).
  - All EQ is minimum-phase IIR (TPT SVF). Linear-phase EQ is offline/batch only.
  - The STFT noise gate is excluded from the chain outside the *Quality* profile.
- **Verification:** each module's `latencySamples()` is unit-tested against a delayed impulse. The chain test enforces the per-profile totals.

### A3. Latency that changes at runtime
- **Symptom:** clicks and echoes when toggling modules, and slowly drifting A/V sync.
- **Root cause:** bypassing a module with latency removes its delay; changing look-ahead mid-stream shifts audio in time.
- **Solution:**
  - `ModuleSlot` keeps the dry path delayed by the module's latency and crossfades over 20 ms. It pre-rolls re-enabled modules so look-ahead lines are primed.
  - The global bypass uses a dry path delayed by the full chain latency.
  - Latency-affecting settings are **structural**, changeable only through the latency profile, which triggers an explicit re-prepare (`ProcessingChain::needsReprepare()`).
- **Verification:** "ModuleSlot: bypassed slot is a pure latency-compensated delay…" and "Chain: everything bypassed = input delayed…" (bit-transparency < 1e-6).

### A4. Lip-sync for video
- **Symptom:** dialogue feels late in films and cut-scenes.
- **Solution:** keep added latency far below perceptual thresholds. ITU-R BT.1359 puts detectability at roughly 45 ms audio-early and 125 ms audio-late. *Balanced* (~4 ms algorithmic + I/O) is well inside that, and even *Quality* (~25 ms) is acceptable for video.

### A5. Bluetooth headphones
- **Symptom:** "Flubsound adds 200 ms." In fact, the Bluetooth codec does.
- **Root cause:** SBC/AAC/aptX pipelines add 100–300 ms. Lossy codecs also reconstruct peaks *above* the input true peak.
- **Solution:**
  - Detect Bluetooth endpoints: show the codec latency separately and suggest the *Bluetooth Headphones* device preset.
  - That preset uses a **−2 dBTP** ceiling to leave room for codec overshoot.

### A6. Hidden resampling
- **Symptom:** extra latency plus a slight high-frequency haze.
- **Root cause:** in shared mode the Windows audio engine resamples to the endpoint mix format; if the virtual endpoint, engine and physical device disagree, audio is converted twice.
- **Solution:** default every Flubsound endpoint and the engine to the physical device's mix rate (usually 48 kHz). The virtual driver advertises the same format, and a mismatch is flagged in the UI.

---

## B. Clipping, distortion & gain staging

### B1. Inter-sample peaks
- **Symptom:** a "0 dBFS"-safe master still distorts in DACs, lossy encoders and Bluetooth.
- **Root cause:** the reconstructed analog waveform exceeds the sample peaks by up to ~3 dB for high-frequency content.
- **Solution:** `TruePeakLimiter` detects on a 4× polyphase-interpolated signal (BS.1770-4 Annex 2 method). A sliding-minimum + box-filter gain envelope provably reaches the required gain before the peak leaves the look-ahead. The default ceiling is −1 dBTP (−2 for Bluetooth), with a final counted safety clamp.
- **Verification:** limiter tests drive +20 dB noise, square-ish waves, impulses and 11 kHz sines at 44.1/48/96 kHz. They check sample peak ≤ ceiling and an *independent* FFT-interpolated true peak ≤ ceiling + 0.1 dB, with `getSafetyClipCount() == 0`.

### B2. Boost stacking
- **Symptom:** EQ + bass + exciter + saturation + macros add up to 20 dB and everything slams the limiter.
- **Solution (gain staging by construction):**
  1. Saturation, harmonics and exciters have **unity small-signal gain** (`f(gx)/g`), so they change timbre, not level.
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
  - The air exciter band-limits its input to ≤ 7 kHz and uses polynomial order ≤ 3, so all products stay below 21 kHz without oversampling.
  - The psychoacoustic bass waveshaper operates on content below ~250 Hz, so its harmonics are nowhere near Nyquist.
  - Antiderivative anti-aliasing (ADAA) is on the roadmap for a zero-latency clipper option.
- **Verification:** saturator test (15 kHz tone at 48 kHz, aliases < −60 dB) and clarity test (no alias products at 44.1 kHz).

### B4. Limiter pumping and "flattening"
- **Symptom:** music breathes; after a gunshot everything ducks for half a second.
- **Solution:**
  1. A soft clipper handles sub-millisecond peaks, so the limiter sees fewer, longer overs.
  2. *Glue*: 3-band pre-compression with phase-coherent LR4 splitting, so bass does not drive broadband gain reduction.
  3. **Program-dependent release**: fast (release/5) for isolated peaks, slow only after limiting has been continuous for more than 50 ms.
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
- **Solution:** every dynamics processor detects on the max across all channels and applies identical gain: compressor, limiter, dynamic EQ, transient shaper and bass protection. The DynamicEq test "stereo link" asserts that the quiet channel gets the loud channel's gain.

### C2. Compression destroys distance cues
- **Symptom:** far footsteps sound as close as near ones.
- **Root cause:** loudness is a primary distance cue, and heavy compression flattens it.
- **Solution:**
  - Instead of global squashing, Gaming uses **targeted** processing: upward compression of *quiet high-frequency detail* only (footstep band 3.2 kHz, `BoostBelow`) and **anti-masking** of very loud lows (explosions).
  - The upward compressor has a noise-floor taper and a gentle ratio.
  - The *Competitive FPS* preset keeps broadband compression off.

### C3. Wideners and crossfeed corrupt localisation
- **Symptom:** blurry or phasey positions, and "inside-the-head" confusion.
- **Solution:**
  - Width is applied to the side signal only, and only above 180 Hz.
  - Gaming mode forces crossfeed to 0.
  - When the virtualiser produced binaural output, width, space and crossfeed are **locked off** (binaural lock). Only the ILD-emphasising *positional focus* remains.

### C4. Double HRTF
- **Symptom:** muffled, comb-filtered, "far away" audio.
- **Root cause:** the game (or Windows Sonic / Dolby Atmos for Headphones) already renders binaural, and Flubsound virtualises again.
- **Solution:** the virtualiser only runs on 5.1/7.1 input, never on stereo. Onboarding explains: *either* the game's HRTF with a stereo endpoint, *or* the game in 7.1 plus Flubsound's virtualiser, never both. The *7.1 Headphone Surround* preset documents this.

### C5. Masking after loud events
- **Symptom:** footsteps inaudible for a second after a grenade.
- **Solution:** a fast auto-release limiter, the anti-masking low-shelf dynamic-EQ band (gaming band 6), per-band glue, and a fast bass-protection release (150 ms).

### C6. Front/back confusion in virtual surround
- **Symptom:** rear sounds are heard in front.
- **Root cause:** a spherical head is front/back symmetric.
- **Solution:** a rear pinna-shadow high shelf (−4 dB at 4 kHz for |azimuth| > 90°), head-radius personalisation, early reflections for externalisation, and measured HRIRs (SOFA) as the HQ renderer on the roadmap.

### C7. Anti-cheat and tournament rules
- **Symptom:** the game refuses to start, or the player is flagged.
- **Solution:**
  - Flubsound **never injects into or hooks game processes**. It only receives audio the OS delivers to its virtual endpoints.
  - The driver is properly signed (attestation → WHQL) and HVCI-compatible.
  - Some tournaments restrict third-party audio processing, so the *Tournament Clean* preset limits processing to ceiling protection, and a single hotkey toggles full bypass.

### C8. Voice chat problems
- **Symptom:** your team hears echo, or your voice pumps.
- **Solution:** chat is a separate strip with its own profile, and the mic is not routed through music/game processing. The Windows communications default device is set to the Chat endpoint so OS ducking applies only there.

---

## D. Platform issues

### D1. Clock drift between clock domains (Windows / macOS / Linux)
- **Symptom:** a click every few minutes, or slowly growing latency.
- **Root cause:** the virtual endpoint (system timer) and the DAC crystal run at slightly different rates (±100 ppm is common).
- **Solution:** `DriftCompensatedFifo`, an SPSC FIFO feeding an adaptive cubic resampler whose ratio is steered by a PI controller on the FIFO fill level (target: 2 blocks). The driver design also allows *clock slaving*, where engine pulls advance the virtual endpoint position, removing drift at the source.
- **Verification:** simulated ±200 ppm drift over long runs gives no underruns or overruns and a bounded fill level (app unit tests; roadmap soak test).

### D2. Exclusive mode locks everyone else out (Windows)
- **Solution:** default to shared low-latency mode (`IAudioClient3`). Offer exclusive mode only when the virtual endpoints are the system default, so all apps flow through Flubsound.

### D3. Device changes, hot-plug, sleep/resume
- **Solution:** keep the virtual endpoint as the Windows default so applications never see the physical device disappear. The engine follows device changes (JUCE device-change detection plus `IMMNotificationClient`) and reopens after resume. Settings persist the device by name and ID, with fallback.

### D4. Double processing by OEM "enhancements"
- **Symptom:** harsh or boomy sound, or effects stacked on effects.
- **Root cause:** OEM APOs (e.g. laptop "audio enhancements"), Windows Spatial Sound or loudness equalisation are active on the physical endpoint.
- **Solution:** detect and warn in onboarding with a direct link to the endpoint's properties, and recommend disabling enhancements on the output device Flubsound feeds.

### D5. Power management (Windows 11 EcoQoS, hybrid CPUs)
- **Symptom:** dropouts when the window is minimised.
- **Root cause:** Windows parks background processes on efficiency cores and throttles timers.
- **Solution:** `SetProcessInformation(ProcessPowerThrottling)` opt-out plus MMCSS "Pro Audio" on the audio thread (`PlatformServices_win.cpp`).

### D6. Driver signing & security features
- **Symptom:** "driver blocked", or the install fails with Secure Boot / HVCI / Smart App Control.
- **Solution:** EV code-signing certificate, Microsoft attestation signing via Partner Center (WHQL later for broad OEM trust), HVCI-compatible driver code (no executable pool allocations, no writable+executable sections), and installer checks with clear messages.

### D7. Undocumented per-app routing API
- **Symptom:** per-app assignment silently stops working after a Windows update.
- **Solution:** the adapter is isolated behind `FLUB_ENABLE_UNDOCUMENTED_ROUTING`, checks the Windows build, verifies results by reading them back, and falls back to opening `ms-settings:apps-volume` with guidance. Process-loopback capture offers a documented monitoring path.

### D8. macOS permissions and installation
- **Symptom:** no audio from process taps, or the virtual device is missing.
- **Solution:**
  - Request the audio-capture TCC permission with a clear purpose string.
  - The HAL plug-in is installed by a signed, notarised `.pkg` into `/Library/Audio/Plug-Ins/HAL`, followed by a `coreaudiod` restart.
  - Universal binaries (arm64 + x86_64).
  - Process taps require macOS 14.2+; older systems fall back to the virtual device with manual routing.

### D9. Linux diversity
- **Symptom:** high latency or xruns.
- **Solution:** document and set the PipeWire quantum for Flubsound nodes, use rtkit for realtime priority (best-effort `SCHED_FIFO` fallback), use portals for Flatpak, and prefer the JACK or PipeWire-JACK device type.

### D10. ASIO specifics
- ASIO drivers are often single-client. When ASIO is the output, Flubsound is that client and every application must route through Flubsound's virtual endpoints. The UI states this explicitly.

---

## E. Engineering & real-time software

| Pitfall | Solution in Flubsound |
|---|---|
| Allocation / locks / logging on the audio thread (priority inversion, page faults) | A written RT contract in `Processor.h`. `AllocationGuard` tests over every module and the whole chain (0 allocations, including mode and A/B switches). Clang RealtimeSanitizer option (`FLUB_RTSAN`) in CI. |
| GUI ↔ audio data races | Only three channels: `ParameterStore` (relaxed atomics, two banks), `MeterBus` (atomics) and `AnalyzerTaps` (SPSC rings). No shared mutable objects and no audio-thread callbacks into the GUI. |
| Object lifetime when swapping engines/HRIR sets | Structural changes happen in `prepare()` off the audio thread. The documented roadmap design is a double-buffered engine swap with deferred (RCU-style) reclamation on the message thread. |
| Zipper noise | All continuous parameters are smoothed. Discrete changes (filter type, bypass) crossfade. Coefficients are updated at a 16-sample control rate using modulation-safe SVFs. |
| Block-size dependence | Every module is tested for identical output with 1, 7, 64 and 512-sample blocks (control counters carry across blocks). |
| Preset breakage between versions | String keys, versioned files, unknown keys ignored, missing keys defaulted, choices stored as labels. |
| Flaky "golden file" audio tests | Property-based assertions instead: response matches the analytic curve, mono sum is preserved, ceiling holds, latency is exact, output is finite. Golden renders are used only for regression diffs with tolerances. |

---

## F. Product & safety

| Pitfall | Solution |
|---|---|
| "Louder = better" bias hides bad processing | Loudness-matched global bypass (the dry path is matched to the processed loudness and never pushed above the ceiling), plus A/B banks. |
| Hearing damage from boosted loudness | Output loudness readouts and a hearing-guard notice when sustained high short-term loudness is detected. Defaults are conservative (Boost 0 %, ceiling −1 dBTP). WHO safe-listening guidance (≈ 80 dB(A) for 40 h/week) is cited in onboarding. |
| Users stack Flubsound with other enhancers | Onboarding checks for other enhancement software, OEM APOs and spatial sound, with guidance to disable them. |
| Too many knobs | A three-level UI: Boost Intensity + 5 mode macros → module cards with key controls → full parameter lists. Presets set base values and macros add staged contributions on top. |
