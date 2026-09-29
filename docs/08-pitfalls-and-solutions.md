# 08 — Major Pitfalls & Concrete Solutions (Workflow 6)

Each pitfall below lists:
- **Symptom**: what users or testers experience.
- **Root cause**.
- **Solution**: what Flubsound does, pointing at the code or design that implements it.
- **Verification**: how we prove it stays fixed.

The sections below are grouped by engineering area (A–F). The brief asks for pitfalls in four areas: **latency, stability, anti-cheat and audio quality**. This table maps each of them to the sections that cover it:

| Brief area | Sections | What they cover |
|---|---|---|
| **Latency** | [A1](#a1-buffers-stack-up-silently)–[A6](#a6-hidden-resampling), [D2](#d2-exclusive-mode-locks-everyone-else-out-windows), [D9](#d9-linux-diversity) | The latency budget and its scopes, look-ahead creep, constant latency under bypass, lip-sync, Bluetooth, hidden resampling, WASAPI modes, the PipeWire quantum |
| **Stability** (no dropouts, crashes or runaway state) | [B6](#b6-naninf-poisoning), [B7](#b7-denormals), [D1](#d1-clock-drift-between-clock-domains-windows--macos--linux), [D3](#d3-device-changes-hot-plug-sleepresume), [D5](#d5-power-management-windows-11-ecoqos-hybrid-cpus), [D7](#d7-undocumented-per-app-routing-api), [D10](#d10-asio-specifics), [E](#e-engineering--real-time-software) | NaN/Inf and denormals, clock drift and xruns, device changes, power management, fragile OS APIs, real-time contract, data races, object lifetime |
| **Anti-cheat** | [C7](#c7-anti-cheat-and-tournament-rules), [D6](#d6-driver-signing--security-features), [D7](#d7-undocumented-per-app-routing-api) | What Flubsound touches (and never touches) in a game process, capture and routing APIs, hotkeys, driver signing |
| **Audio quality** | [B1](#b1-inter-sample-peaks)–[B6b](#b6b-real-time-levelling-during-pauses-and-fade-outs), [C1](#c1-unlinked-dynamics-move-sounds)–[C6](#c6-frontback-confusion-in-virtual-surround), [C9](#c9-headset-on-board-dsp-stacking-with-flubsound-eg-turtle-beach-superhuman-hearing), [A6](#a6-hidden-resampling), [D4](#d4-double-processing-by-oem-enhancements), [F](#f-product--safety) | Inter-sample peaks, boost stacking, aliasing, pumping, DC, levelling, positional cues in games, double HRTF, stacked headset or OEM processing, fair (loudness-matched) comparison |

Engineering-process, product and safety pitfalls (E, F) follow the platform section.

---

## A. Latency

### A1. Buffers stack up silently
- **Symptom:** "It adds 40 ms and ruins my aim." Each layer looks small, but together they are not.
- **Root cause:** added latency is the sum of the virtual endpoint period, capture safety margin, processing block, algorithmic look-ahead, output device buffer and any sample-rate converters.
- **Solution:**
  1. One explicit **latency budget** with labelled scopes (`01-architecture.md` §5). **Chain** (per strip, algorithmic): *Low Latency* 100 samples ≈ 2.1 ms, *Balanced* 192 samples = 4.0 ms, *Quality* ≈ 28 ms for music/batch only. **App engine**: the desktop app's master limiter adds 44 samples (0.9 ms; 0.5 ms look-ahead when every strip runs Low Latency) or 68 samples (1.4 ms) otherwise. **Added end-to-end** (estimate, Windows driver path): ≈ 9.5 ms in *Low Latency*, the profile that meets the ≤ 10 ms target, and ≈ 12–13 ms in *Balanced*, the upper edge of R1.1. Today's capture paths add their own buffering on top (`01-architecture.md` §5.3): the Windows process-loopback FIFO targets 612 frames (12.75 ms) with 10 ms capture packets and 128-frame blocks, and a PipeWire null sink adds up to one quantum (≈ 21 ms at the common default of 1024).
  2. **Event-driven** WASAPI with small periods: `IAudioClient3` low-latency shared mode, or exclusive mode. Low-latency shared mode is the default when no device choice is saved: the app opens JUCE's *Windows Audio (Low Latency Mode)* type, falling back to *Windows Audio* if the device cannot be opened in that mode. A type picked in Settings > Audio is saved and wins.
  3. The **driver shared-memory path** (design), which reads the virtual endpoint's cyclic buffer directly instead of paying for a second WASAPI capture hop (`platform/windows/driver/`).
  4. A 128-frame block at 48 kHz.
  5. The latency shown in the UI is the *reported* total: device input + device output latency (as the driver reports them) + engine latency + the capture FIFO target. It is not measured.
- **Verification:** `tests/test_engine.cpp` "Chain: latency per profile and constant under module bypass" asserts Balanced < 5 ms and Low Latency < 2.5 ms chain latency, and "MixEngine: master look-ahead follows the strips' latency profiles (0.5 ms only when all are Low Latency)" asserts the exact app engine totals (144 and 260 samples at 48 kHz). The end-to-end figures are estimates: a loopback-impulse measurement tool (roadmap Phase 1, item 1.2) and device-lab test 8 (`10-headset-compatibility.md` §5) will measure real added latency per device.

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
  - Latency-affecting settings are **structural**, changeable only through the latency profile (or by installing a neural model, whose latency joins the chain's), which triggers an explicit re-prepare (`ProcessingChain::needsReprepare()`).
  - In the desktop app that re-prepare is a crossfaded engine swap ([01 §3](01-architecture.md#3-process--thread-model)). The new engine is built off the audio thread and pre-rolled beside the old one. The old engine then fades out over 10 ms and the new one fades in over 10 ms, so the time shift happens inside that dip. A layout change at equal latency crossfades with equal gain. Neither case uses an equal-power fade: two copies of the same programme add up to +3 dB with one, past the master limiter's ceiling.
- **Verification:** "ModuleSlot: bypassed slot is a pure latency-compensated delay and toggling is click-free" (error ≤ 1e-5), "Chain: everything bypassed = input delayed by the chain latency (bit-transparent path)" (error ≤ 1e-6), "App: engine swap on a latency-profile change and a layout change mid-stream: no click, no gap longer than the fade", and "App: neural model install and removal mid-stream go through the swap (model in the new engine, no click, no gap)". In the last two tests, no step exceeds the sine's own step plus the fade slope, and no near-silence lasts longer than one 10 ms fade.

### A4. Lip-sync for video
- **Symptom:** dialogue feels late in films and cut-scenes.
- **Solution:** keep added latency far below perceptual thresholds. ITU-R BT.1359 puts detectability at roughly 45 ms audio-early and 125 ms audio-late. *Balanced* (~4 ms algorithmic + I/O) is well inside that, and even *Quality* (~28 ms algorithmic) is acceptable for video. The Quality figure holds at every rate: its look-aheads and gate frame are defined in ms, and below 32 kHz (Bluetooth hands-free) Quality runs as Balanced, where it used to reach 144 ms at 8 kHz ([11 E42](11-enhancement-report.md#e42) E42a; tests *Chain: Quality runs as Balanced below 32 kHz, without a re-prepare* and *Chain: each profile's latency in ms is about the same from 44.1 to 192 kHz*). Strips that carry one A/V programme share a sync group (`StripConfig::syncGroup`) and are padded to its slowest strip; other strips are not padded, so a Quality Music strip never delays the Game strip (test *MixEngine: padding only within sync groups, per-strip latency reported*).

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
  4. The **SafetyGovernor** scales every loudness-adding macro contribution back when average limiter gain reduction is below −6 dB or the measured THD+N of the saturator and the soft clipper is above −30 dB. It recovers slowly, with hysteresis.
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
- **Solution:** the loudness control loops AutoLevel and AutoDrive use `GatedLoudness`. The slow 3 s measure only advances while programme is present: block RMS above −70 dBFS, a fast 100 ms follower above −50 LUFS, and within 20 LU of the slow value (a programme held out by that relative gate for 3 s restarts the slow measure, so a much quieter next source is not ignored for good). Adaptation is additionally slew-limited to +1 dB/s up and −4 dB/s down. AutoLevel also holds its gain while a 400 ms loudness reads more than 8 LU above the slow value, so a loud event does not leave a hole in the quiet programme after it (upper gate, [11 E21](11-enhancement-report.md#e21)). The bypass loudness match counts only 100 ms sub-blocks with programme over a 3 s window.
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
  - Instead of global squashing, Gaming uses **targeted** processing: a *cue enhancer* that lifts only what rises out of the ambience in the footstep bands (3.2 kHz and 260 Hz, `CueLift`, [11 E19](11-enhancement-report.md#e19)), so a step keeps its level relative to the bed and to other steps, and **anti-masking** of very loud lows (explosions; a user dynamic-EQ band in the presets that use it).
  - The upward compressor has a noise-floor taper and a gentle ratio.
  - When only the Gaming *Detail* macro switches the compressor on and no ratio was chosen, the chain runs it at 1:1: it only lifts quiet sounds, and gunshots and explosions keep their dynamics (03 §9.7; test *Gaming: a compressor switched on only by a macro is upward-only - loud sounds keep their dynamics unless a ratio was chosen*). The *Competitive FPS* preset sets 1:1 explicitly; other Gaming presets choose a gentle downward ratio on purpose.

### C3. Wideners and crossfeed corrupt localisation
- **Symptom:** blurry or phasey positions, and "inside-the-head" confusion.
- **Solution:**
  - Width is applied to the side signal only, and only above `spatial.lowCut` (default 180 Hz; the mono sum is preserved exactly).
  - Gaming mode forces crossfeed to 0.
  - When the virtualiser produced binaural output, width, space and crossfeed are **locked off** (binaural lock). Only the ILD-emphasising *positional focus* remains.
  - Positional focus has a **polarity guard**: it lifts the side signal around 3 kHz only as far as the mid signal in that band allows, so it never puts an anti-phase copy in the far ear. A source panned *hard* to one side keeps its infinite ILD at *Positional* 100 %, partially panned sources gain ILD (R = L/2: 6 → 18 dB at 3 kHz), and the mono sum is unchanged ([03 §7.3.2](03-dsp-design.md#732-positional-focus); test *StereoSpatializer: positional focus never flips the far ear - hard-panned sources stay hard-panned*).
  - Known limitations: width above 1 still puts an anti-phase copy of a hard-panned source in the far ear (at the default minimum correlation of 0 the mono safety pulls such width back to 1), and the guard works on the band mix, so a hard-panned sound under a louder centred one can still be lifted ([03 §7.9](03-dsp-design.md#79-known-limitations)).

### C4. Double HRTF
- **Symptom:** muffled, comb-filtered, "far away" audio.
- **Root cause:** the game (or Windows Sonic / Dolby Atmos for Headphones) already renders binaural, and Flubsound virtualises again.
- **Solution:** the virtualiser only runs on 5.1/7.1 input, never on stereo. The rule is *either* the game's HRTF with a stereo endpoint, *or* the game in 7.1 plus Flubsound's virtualiser, never both. The *7.1 Headphone Surround* preset description and the headset device-profile advice say so; an onboarding page that explains it is roadmap (`07-roadmap.md` item 1.6).

### C5. Masking after loud events
- **Symptom:** footsteps inaudible for a second after a grenade.
- **Solution:** a fast auto-release limiter (release/5 for isolated peaks), the anti-masking low-shelf dynamic-EQ band (90 Hz, cut above −22 dBFS; user band 0 in the gaming presets that tame loud LF, no longer scaled by *Footsteps* since [11 E20](11-enhancement-report.md#e20)), the footstep cue enhancer, whose background falls back with a 400 ms time constant after the event, so the next step is lifted again (step lift 1–2 s after the combat scene within 0.4 dB of before in `tests/test_scenes.cpp`), per-band glue where a preset arms it (e.g. *Cinematic Adventure*; no Gaming macro raises glue), and a fast bass-protection release (150 ms).

### C6. Front/back confusion in virtual surround
- **Symptom:** rear sounds are heard in front.
- **Root cause:** a spherical head is front/back symmetric.
- **Solution:** a rear pinna-shadow high shelf (−4 dB at 4 kHz for |azimuth| > 90°), head-radius personalisation (`virt.headRadius`, 70–105 mm), early reflections for externalisation (`virt.room`), and measured HRIRs as the HQ renderer. The core already has a direct-form HRIR renderer (`HeadphoneVirtualizer::setHrirSet`, capped at 1024 taps); a SOFA loader, HRIR resampling and partitioned FFT convolution are on the roadmap.

### C7. Anti-cheat and tournament rules
- **Symptom:** the game refuses to start, the player is kicked, or the account is flagged.
- **Root cause:** kernel-level anti-cheats (for example Easy Anti-Cheat, BattlEye, Riot Vanguard, FACEIT) look for code injected into the game, hooks on its APIs, memory reads of its process, input hooks and overlays, and vulnerable or test-signed kernel drivers. Audio tools that work by injecting a DLL into the game or hooking its audio API look exactly like that.
- **Solution: Flubsound stays outside the game process.** Everything below describes the code as it is, except where it says *design*.
  - **No injection, no hooks, no overlay.** Flubsound loads no DLL into another process, installs no API, window or input hooks and draws no in-game overlay; its only windows are its own (main window, dialogs, tray icon, and a message-only window that receives hotkeys). It never reads or writes another process's memory. It processes only audio the OS audio engine delivers to it.
  - **Per-process loopback capture** (Windows 10 build 19041+ / Windows 11, `ProcessLoopbackCapture` in `app/Source/platform/PlatformServices_win.cpp`) is a documented audio-engine feature: `ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` names the target by **process id**, and the Windows audio service hands Flubsound a copy of that process's (by default its whole process tree's) render audio. The capture needs no handle to the game and nothing is read from its memory. Separately, Flubsound opens a `PROCESS_QUERY_LIMITED_INFORMATION` handle, one of the most restricted process access rights (it cannot read memory, inject or control the process), to read an executable's path for the session list and to check that a capture target exists. Some anti-cheats filter or log handle access to the protected game process; whether any of them objects to a limited-query handle has not been verified yet (QA matrix, roadmap item 2.12).
  - **Per-app endpoint routing** through the undocumented `IAudioPolicyConfigFactory` (the interface behind Windows' own per-app output setting) is compiled in only with `FLUB_ENABLE_UNDOCUMENTED_ROUTING` (off by default) and runs only on explicit user action (D7). It is a user-mode call into the audio service's endpoint policy, the same setting the user can change in Settings; it does not touch the game process. Default builds open `ms-settings:apps-volume` instead.
  - **Global hotkeys** use `RegisterHotKey` on Windows (a message-only window receives `WM_HOTKEY`), Carbon `RegisterEventHotKey` on macOS and, on Linux, `XGrabKey` on the root window under X11 or the xdg-desktop-portal GlobalShortcuts interface in Wayland sessions. There is no low-level keyboard hook (`WH_KEYBOARD_LL`), no event tap, no raw-input monitoring and no X11 snooping (XRecord or XInput raw events), so Flubsound never sees keystrokes other than its own chords: an X key grab delivers only the grabbed chord, and the portal only reports which bound shortcut was pressed.
  - **Driver path (design).** The planned "Flubsound Virtual Audio" driver is a WaveRT virtual audio device: to Windows and to games it is an ordinary audio endpoint, like a USB headset. It has to be signed properly: Microsoft attestation signing via Partner Center, WHQL / HLK later, and HVCI-compatible (`platform/windows/driver/README.md` §8). Two rules matter for anti-cheat:
    - Test-signing mode (`bcdedit /set testsigning on`), used for driver development, is refused by the major kernel anti-cheats, so test-signed builds stay on dedicated development VMs and gaming QA only ever uses properly signed builds.
    - The driver must never qualify for the Microsoft vulnerable driver blocklist (enforced by Windows with memory integrity / HVCI and Smart App Control; anti-cheats are known to keep similar lists of their own). Its control IOCTL maps only the driver's own dedicated pages, validates every length and index, and never returns a kernel pointer or exposes arbitrary memory (driver README §8, "Never become a BYOVD primitive").
  - **Tournament rules.** Some tournaments restrict third-party audio processing, so the *Tournament Clean* preset limits processing to ceiling protection, and a single hotkey (default Ctrl+Alt+F; Windows, macOS and Linux under X11) toggles full bypass on every strip.
- **Verification:** nothing here is proven by a test yet. The anti-cheat compatibility matrix (EAC, BattlEye, Vanguard, VAC across the top competitive titles) is roadmap item 2.12 with the Phase 2 exit criterion "no anti-cheat conflicts across the top 20 competitive titles". Anti-cheat vendors do not publish their rules, so the points above lower the risk; they do not guarantee compatibility.

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
- **Solution:** `DriftCompensatedFifo` (`app/Source/engine/`), an SPSC FIFO feeding an adaptive cubic Hermite resampler whose ratio is steered by a PI controller on the FIFO fill level (target: 2 device blocks, raised to the largest recent capture burst + 1 block; critically damped at 0.15 rad/s; correction clamped to ±0.5 %). It serves the per-app capture streams, which are requested at the device rate so the nominal ratio is 1 (the Hermite interpolator does not band-limit); a device rate change restarts every running capture at the new rate rather than resampling, for example, 48 kHz into a 16 kHz hands-free device (`tests/app/test_app_device_safety.cpp`). The driver design also allows *clock slaving*, where engine pulls advance the virtual endpoint position, removing drift at the source.
- **Verification:** automated in simulation, `tests/test_drift_fifo.cpp` (it compiles `DriftCompensatedFifo.cpp` into `flub_tests`, so it runs on every OS). An event-driven model pairs a capture clock at 48 kHz × (1 + drift) with a device clock at exactly 48 kHz:
  - "DriftFifo: +-200 and +-2000 ppm drift, 10 ms and 441-frame packets, 128 and 512 blocks: settles clean": for every combination, after the loop settles, no underruns, overflows or dropped frames, the fill stays within ±1 ms of the target, the learned correction equals the true drift within 5 ppm, the output never steps more than a clean sine (no skipped or repeated frames), and `push()` / `pull()` never allocate;
  - "DriftFifo: a capture stall is one counted underrun, then the stream recovers" (200 ms stall: exactly one underrun, click-free fade-out and fade-in, the learned drift survives the re-prime);
  - "DriftFifo: a device stall drops the oldest audio (counted overflow), then the stream recovers" (300 ms and 1.5 s stalls: one counted overflow, the stalled audio dropped, back at the target fill);
  - "DriftFifo: 7.1 capture into a stereo FIFO is downmixed per ITU-R BS.775 (LFE dropped, -3 dB)" and "DriftFifo: a mono capture is duplicated to both channels of a stereo FIFO".

  Not covered: real devices and long runs. The soak test (Phase 1, items 1.1 and 1.10) and driver clock slaving (design) remain.

### D2. Exclusive mode locks everyone else out (Windows)
- **Solution (partly roadmap):** default to shared low-latency mode (`IAudioClient3`, JUCE's *Windows Audio (Low Latency Mode)*). Without a saved device choice the app opens that type, falling back to *Windows Audio* if the device cannot be opened in it; a mode chosen in Settings > Audio is saved and kept. Offering exclusive mode only when the virtual endpoints are the system default (so all apps flow through Flubsound) is roadmap.

### D3. Device changes, hot-plug, sleep/resume
- **Solution:** with the virtual driver (design), keep the virtual endpoint as the Windows default so applications never see the physical device disappear. The engine follows device changes through JUCE's device-change notifications (no separate `IMMNotificationClient`). Settings persist JUCE's device state (by device name) and the *preferred* output name; when the preferred output disappears JUCE falls back to another device, and `EngineController` switches back once the preferred one is listed again (rescan every 5 s while it is missing). That fallback can pick the input's own loopback partner (in the cable setup the default output is CABLE Input while the input is CABLE Output), a feedback loop through Boost; `AudioEngineHost`'s loopback-pair guard (docs/11 E51 Phase A) holds the output at silence from the first sample of such a device start, freezes the engine and exposes the reason for a banner (`getDeviceSafetyState()`). Tested through JUCE's own device-list fallback with a fake device backend (`tests/app/test_app_device_safety.cpp`). Explicit device selection instead of `selectDefaultDeviceOnFailure`, stable endpoint IDs and event-driven hot-plug remain open (E51 later phases).

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
- **Today:** there is no driver yet, and nothing is signed. CI builds an unsigned Inno Setup installer for the test builds (`installer/windows/FlubsoundPro.iss`, [11 E54](11-enhancement-report.md#e54)): the app into Program Files, the VST3 into `Common Files\VST3`, a Start menu entry and an uninstaller, so SmartScreen warns on the installer and on the app ("More info → Run anyway"). The app and the plug-in are linked with the static C++ runtime, so no Visual C++ Redistributable is needed.

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
- **Today:** CI makes an unsigned, not notarised `.dmg` (and the same files as a `.zip`) with an Applications link to drag the app onto (`installer/macos/make-dmg.sh`, [11 E54](11-enhancement-report.md#e54)); Gatekeeper asks on the first start (right-click → Open). The ad-hoc signature changes with every build, so a TCC grant (microphone) is asked for again after each update until Developer ID signing exists.

### D9. Linux diversity
- **Symptom:** high latency or xruns.
- **Solution:** document the PipeWire quantum (`platform/linux/README.md`: `pw-metadata -n settings 0 clock.force-quantum 256`), prefer the JACK / PipeWire-JACK device type (the server then calls the engine on its own real-time thread), and otherwise promote the audio thread to best-effort `SCHED_FIFO` 20 (or the user's `RLIMIT_RTPRIO`), else ask RealtimeKit over the system D-Bus from the message thread (SCHED_RR at up to its MaxRealtimePriority; [11 E44](11-enhancement-report.md#e44), tested against a mock rtkit only). The native PipeWire node ([11 E48](11-enhancement-report.md#e48), `app/Source/platform/pipewire/`) sets `node.latency` on Flubsound's own node (256/48000, or 128/48000 locked on Low Latency, changed in place without a re-open) and runs the engine on PipeWire's own real-time data thread; it is built and tested against a headless PipeWire but not yet offered in the app's device list. Roadmap: that wiring, and the Flatpak portals.

### D10. ASIO specifics
- ASIO drivers are often single-client. When ASIO is the output, Flubsound is that client and every application must route through Flubsound's virtual endpoints. A UI note that says so is roadmap; ASIO itself is only built when `FLUB_ASIO_SDK_DIR` is set.

---

## E. Engineering & real-time software

| Pitfall | Solution in Flubsound |
|---|---|
| Allocation / locks / logging on the audio thread (priority inversion, page faults) | A written RT contract in `Processor.h`. `AllocationGuard` tests over every module, the whole chain and the mixer (0 allocations, including mode and A/B switches). Clang RealtimeSanitizer in CI (job `rtsan`, `FLUB_RTSAN=ON`, Clang 20): `ProcessingChain::process`, `MixEngine::process` and every `Processor::process` override are `[[clang::nonblocking]]` (`FLUB_NONBLOCKING`, `common/Realtime.h`), so any allocation, free, lock or blocking system call reached from them aborts the full test run. So are the module functions the audio thread calls outside `process()`: every `Processor::reset` override and `TransientShaper::reset` (NaN/Inf recovery, bypass re-activation), and the setters `ProcessingChain::applyParameters` calls once per block (`setParams`, `DynamicEq` / `ParametricEq::setBand`, `ParametricEq::setOutputGainDb`, the `TransientShaper` setters; `TruePeakLimiter::setParams` also from `MixEngine::setMasterCeilingDb`), so tests that call them directly are checked too. `tests/test_rtsan.cpp` checks that the annotations are in place and that the sanitizer really stops a violation. Not covered by RTSan: the app's own callback code. |
| GUI ↔ audio data races | Three main channels: `ParameterStore` (relaxed atomics, two banks), `MeterBus` (atomics) and `AnalyzerTaps` (SPSC rings), plus single atomic hand-offs (effective values, audition mask, strip gain / ceiling). No shared mutable objects and no audio-thread callbacks into the GUI. |
| Object lifetime when swapping engines/HRIR sets | Structural changes happen in `prepare()` off the audio thread. The app swaps whole engines (`AudioEngineHost`), handing them over through two atomic pointers. The audio thread takes the new engine and returns the old one, which is destroyed on the message thread (deferred, RCU-style reclamation). Nothing is freed on the audio thread: 100 swaps are probed with zero allocations, frees and locks. HRIR sets are not swapped at runtime yet. |
| Zipper noise | All continuous parameters are smoothed. Discrete changes (filter type, bypass) crossfade. Coefficients are updated at a 16-sample control rate using modulation-safe SVFs. |
| Block-size dependence | Every module is tested for identical output with 1, 7, 64 and 512-sample blocks (control counters carry across blocks). |
| Preset breakage between versions | String keys, versioned files, unknown keys ignored, missing keys defaulted, choices stored as labels. |
| Field problems that cannot be diagnosed | A rotating log in the user data folder (`Logs/flubsound.log`, 512 KB × 3; no audio, home folder / login / computer names redacted), fed from the message thread only: a 2 Hz poll of the counters the audio thread already keeps in atomics (glitches, safety clips, NaN / Inf drops, overloads) and the device's opens, changes and errors; nothing logs on the audio thread. A crash writes a report into the same folder (POSIX signal handlers on an alternate stack that only use async-signal-safe calls; on Windows an unhandled-exception filter with a minidump); *Settings › Diagnostics › Export diagnostics* zips it all for a report ([11 E54](11-enhancement-report.md#e54), `app/Source/diagnostics/`). |
| Flaky "golden file" audio tests | Property-based assertions instead: response matches the analytic curve, mono sum is preserved, ceiling holds, latency is exact, output is finite. Golden renders are planned only for regression diffs with tolerances (`07-roadmap.md` §7). |

---

## F. Product & safety

| Pitfall | Solution |
|---|---|
| "Louder = better" bias hides bad processing | Loudness-matched global bypass, plus A/B banks. In a bypass comparison the louder side, usually the processed one, is turned down to the other; nothing is raised, so the match needs no headroom and holds within 0.5 LU on hot programme ([11 E37](11-enhancement-report.md#e37); the former capped raise fell 2–3 LU short). The reference passes its own true-peak limiter at the ceiling (test "Chain: matched bypass never overshoots the ceiling when a louder dry peak arrives"). The first flip into bypass is not matched, and bank A/B flips are not matched yet. |
| Hearing damage from boosted loudness | Output loudness readouts (momentary / short-term / integrated LUFS, true peak) exist, and defaults are conservative: a fresh strip loads a factory default with Boost at most 35 % (Signature 35 %, First Run – Game 20 %, Voice Chat 15 %, [11 E36](11-enhancement-report.md#e36)) under a −1 dBTP ceiling. Roadmap (`07-roadmap.md` items 2.11 and 1.6): a hearing-guard notice when sustained high short-term loudness is detected, and WHO safe-listening guidance (≈ 80 dB(A) for 40 h/week) cited in onboarding. |
| Users stack Flubsound with other enhancers | Today: device-profile advice for headsets with their own DSP (C9). Roadmap: onboarding checks for other enhancement software, OEM APOs and spatial sound, with guidance to disable them. |
| Too many knobs | A three-level UI: Boost Intensity + 5 mode macros → module cards with key controls → full parameter lists. Presets set base values and macros add staged contributions on top. |
