## Macros, Music/Gaming modes & protection loops

> Source: `core/src/engine/MacroMap.cpp`, `core/src/engine/ProcessingChain.cpp`, `core/src/engine/Protection.cpp`.

### M.1 Base values, effective values and the macro formula

Presets and the GUI only write **base** values. Each audio block computes **effective** values from them:

```
effective[p] = clamp_p( base[p] + Σ_e  amount_e · c_e(v_e) · g_e )

  v_e      = value of the entry's source: Boost Intensity or Macro 1..5 (0..1)
  c_e(v)   = smoothstep(start_e, end_e, v) ^ exponent_e      (smoothstep(t) = 3t² − 2t³)
  g_e      = SafetyGovernor scale (0.3..1) if the entry is "governed", else 1
  clamp_p  = the parameter's own [min, max] from the layout
```

- Toggles (module enables) can be macro targets. An entry `{ source, XOn, +1, 0, 0.02 }` switches a module on as soon as its macro leaves zero; for example *Warmth* engages Saturation.
- Entries whose effect is "more loudness or drive" are **governed**. The governor can take them back without touching tonal or spatial entries.
- Staggered `start/end` windows make one slider behave "intelligently", because different processes arrive in a musically sensible order.

### M.2 Boost Intensity staging

| Stage (slider position) | Music mode adds | Gaming mode adds |
|---|---|---|
| 0 → 50 % | Presence +0.35, width +0.2, air +0.3 (10–60 %), transient attack +2 dB (10–60 %) | Presence +0.3, positional focus +0.3 (0–60 %), upward-compression detail +5 dB (10–70 %), attack +2 dB (20–70 %) |
| 20 → 90 % | Bass boost +5 dB* (20–80 %), harmonic bass +0.3* (35–90 %) | Bass boost +3 dB* (30–90 %) |
| 30 → 100 % | Maximizer drive +8 dB* (30–100 %, curve^1.2), glue +0.3 (40–100 %) | Maximizer drive +6 dB* (30–100 %, curve^1.2) |
| 60 → 100 % | Saturation drive +4 dB* | — |

\* governed. Boost Intensity also engages the Dynamic EQ (so the Music "de-boom" band tracks it), the Maximizer (from 25 %) and, in Gaming, the Compressor (from 5 %, for upward compression).

### M.3 Music mode macros (Macro 1–5)

| Macro | Targets (amount at 100 %, active window) | Internal dynamic-EQ companion |
|---|---|---|
| **Punch** | Clarity on; transient attack +6 dB (0–100 %); bass tighten +0.5 (20–100 %) | — |
| **Width** | Stereo on; width +0.6 (0–100 %); space +0.35 (40–100 %) | — |
| **Clarity** | Clarity on; presence +0.8; air +0.7 (20–100 %); de-mud +0.5 (0–70 %); Dynamic EQ on | Band 4: de-harsh bell 3.5 kHz, Q 1.2, *cut above* −22 dBFS, 3:1, range 3 dB × Clarity. Band 5: air high shelf 12 kHz, *boost below* −45 dBFS, range 3 dB × Clarity |
| **Loudness** | Maximizer on; drive +10 dB* (curve^1.3); glue +0.5 (30–100 %) | — |
| **Warmth** | Saturation on; saturation drive +9 dB*; harmonic bass +0.2* (40–100 %); bass boost +2 dB* (30–100 %); Bass on | — |

Always-on Music companion: band 6 is a de-boom bell at 120 Hz, *cut above* −14 dBFS, 2.5:1, with range 4 dB × Boost Intensity. When the bass is boosted, boomy passages are held in check dynamically.

### M.4 Gaming mode macros (Macro 1–5)

| Macro | Targets | Internal dynamic-EQ companion |
|---|---|---|
| **Footsteps** | Dynamic EQ on; Compressor on; upward max gain +3 dB (30–100 %) | Band 4: **footstep detail** bell 3.2 kHz, Q 0.9, *boost below* −42 dBFS, 3:1, range 7 dB × Footsteps (3 ms / 120 ms), floor −75 dBFS. Band 5: **footstep body** bell 260 Hz, Q 1.2, *boost below* −45 dBFS, range 3 dB × Footsteps. Band 6: **explosion anti-masking** low shelf 90 Hz, *cut above* −22 dBFS, 3:1, range 6 dB × Footsteps (10 ms / 250 ms) |
| **Positional** | Stereo on; positional focus +0.9; width +0.25 (30–100 %, above the 180 Hz low cut only) | — |
| **Impact** (explosions, gunshots) | Bass on; bass boost +6 dB*; harmonic bass +0.25* (40–100 %); Clarity on; transient attack +4 dB (20–100 %) | — |
| **Detail** (environment) | Compressor on; upward max gain +8 dB; Clarity on; air +0.4 (20–100 %) | — |
| **Voice & Score** | Clarity on; presence +0.7; de-mud +0.4 (20–100 %); Dynamic EQ on | Band 7: **voice / score** bell 2 kHz, Q 0.7, *boost below* −36 dBFS, 2:1, range 4 dB × Voice |

**Why this footstep design works.**
- Footsteps are quiet, transient and broadband, with their identifying energy around 2–5 kHz (the scuff and tick) and 150–400 Hz (the heel).
- *Boost-below* dynamic bands are upward compressors confined to those regions. They lift quiet events and leave loud ones (gunfire, explosions, which exceed the threshold) untouched, so the mix does not get louder overall.
- The noise-floor taper keeps silence and ambience beds from being pumped up.
- The anti-masking shelf only engages on *very* loud low-frequency events and recovers in about 250 ms, so the steps after an explosion are not buried.
- All detection is stereo-linked, so none of this moves a source's apparent direction.

**Gaming policies enforced by the chain** (independent of preset values):
- Crossfeed is forced to 0: it blurs interaural differences, the main lateral localisation cue.
- **Binaural lock:** when the virtualiser rendered 5.1/7.1 to binaural, width = 1, space = 0 and crossfeed = 0. Positional focus (a mild ILD emphasis in 1–6 kHz on the side channel) stays available.
- Presets for competitive play choose the *Low Latency* profile: 0.5 ms look-aheads and 2× short oversampling, about 1.9 ms algorithmic in total.

### M.5 Protection loops

| Loop | Measures | Acts on | Law |
|---|---|---|---|
| **SafetyGovernor** (THD / over-processing) | Maximizer limiter gain reduction (block minimum) and clipper energy ratio, averaged over ~3 s (clip energy in the power domain) | Scale on all *governed* macro amounts | Over budget (avg GR < −6 dB **or** clip energy > −30 dB): scale −15 %/s, floor 0.3. Comfortably under (1.5 dB hysteresis on both): scale +3 %/s up to 1. |
| **AutoLevel** (LUFS input levelling) | *Gated* K-weighted loudness of the input: a 3 s one-pole that only advances while programme is present (block RMS > −70 dBFS, 100 ms follower > −50 LUFS and within 20 LU of the slow value) | Input gain before the chain | Gain = target − measured, clamped ±12 dB, slew +1 dB/s and −4 dB/s, adapted only while the gate is open. Pauses, track gaps and fade-outs never pump the gain up. Applied as a per-block linear ramp. |
| **AutoDrive** (maximizer loudness target) | Gated loudness of the *output* (same gate) | Maximizer drive | Closed loop with a 0.5 LU dead band; integrates at up to 2 dB/s. It can only **reduce** the requested drive (≤ 0 dB, ≥ −24 dB), so it never makes anything louder than the user or macros asked for. |
| **LoudnessMatch** (fair A/B) | Gated loudness of dry vs processed | Gain on the dry path in global bypass | Gain = wet − dry, clamped ±12 dB, slew 3 dB/s, and additionally capped so that the matched dry reference never exceeds the maximizer ceiling (2 s peak hold). |
| **True-peak ceiling** | 4× interpolated peaks | Limiter gain | Look-ahead sliding-minimum + box-filter envelope reaches the required gain before the peak arrives; a final safety clamp counts any engagement. |
| **Master limiter** (mixer) | Sum of all strips | Master gain | −1 dBTP, 1 ms look-ahead, 50 ms auto release. Engages only when several strips overlap hot. |
| **Input sanitation** | Non-finite samples in the input block | Whole chain | Block dropped (silence) and chain state reset. |

### M.6 Bypass, A/B and latency profiles

- **Per-module bypass** (`ModuleSlot`): a 20 ms equal-gain crossfade against a dry path delayed by the module's latency. A re-enabled module is reset and pre-rolled for latency + 64 samples while still fully dry, so look-ahead lines are primed before anything is heard. The chain latency is identical whether a module is on or off.
- **Global bypass**: a 30 ms crossfade to a dry reference delayed by the full chain latency. With *loudness-matched bypass* on (default), the reference is level-matched (§M.5), so comparisons are about tone and dynamics, not loudness.
- **A/B**: two complete parameter banks in the `ParameterStore`. The switch is one atomic, and all continuous parameters glide.
- **Latency profiles** (structural; changing one re-prepares the chain):

| Profile | Gate (STFT) | Saturator OS | Compressor LA | Clipper OS | Limiter LA | Total @ 48 kHz |
|---|---|---|---|---|---|---|
| Quality | in chain (1024) | 2× HQ (32) | 3 ms (144) | 4× HQ (36) | 2 ms + 12 (108) | 1344 smp ≈ 28 ms |
| Balanced | — | 2× short (16) | 1 ms (48) | 4× HQ (36) | 1.5 ms + 12 (84) | 184 smp ≈ 3.8 ms |
| Low Latency | — | 2× short (16) | 0.5 ms (24) | 2× short (16) | 0.5 ms + 12 (36) | 92 smp ≈ 1.9 ms |
