# Flubsound factory presets

This folder holds the factory preset library: 24 presets in three categories (12 Music, 9 Gaming, 3 Device). It has two parts:

| Folder | Contents |
|---|---|
| `factory/` | `<category>-<slug>.json` preset files. The desktop app embeds them as BinaryData, `flubsound-cli` finds them at run time, and the installer copies them to `share/flubsound/presets/factory`. |
| `devices/device-profiles.json` | Headset and output-device profiles. They add device-aware advice and ceiling caps, and suggest presets by name (for example *Competitive FPS*, *Flubsound Signature* or *Bluetooth Headphones*). |

`tests/test_factory_presets.cpp` checks every file on every build. It confirms the metadata, that every key and choice label is known, that only non-default values are stored and that the ceilings are right. It also renders each preset through the full `ProcessingChain` and checks the output against its ceiling. Details are in the last section.

---

## How a preset works

A preset stores **base values** only. Two further layers are added on top of them in real time (`core/src/engine/MacroMap.cpp`):

* **Boost Intensity** is staged, so each part arrives at a different point:
  * clarity, width and detail come in first;
  * bass and impact come next;
  * maximizer drive and saturation come in last (drive starts at 30 %).
* **The five mode macros:**
  * Music mode: *Punch, Width, Clarity, Loudness, Warmth*.
  * Gaming mode: *Footsteps, Positional, Impact, Detail, Voice & Score*.
  * Several macros also drive internal dynamic-EQ bands, such as the footstep lift, explosion anti-masking, the voice band, de-harsh and de-boom.

Every factory preset sets Boost and the macros to a sensible *starting point*, so the user can go further from there. Anything that adds loudness or drive through Boost or a macro is **governed**: the SafetyGovernor scales it back when the limiter or clipper is working too hard. For this reason the factory presets get their loudness from the governed controls (Boost, Loudness), not from a fixed `max.drive`.

**Latency profiles** (48 kHz, algorithmic, with the current modules):

| Profile | Latency | What it enables |
|---|---|---|
| **Quality** | ~28 ms | Spectral noise gate in the chain, HQ oversampling, longest look-ahead. Still well inside lip-sync tolerance for video, but not for competitive play |
| **Balanced** | ~4 ms | Default. Safe for video lip-sync |
| **Low Latency** | ~2 ms | Short look-ahead and oversampling, for competitive play |

---

## Music (Music mode)

| Preset | Intent | Key settings (base values) | Boost / macros | Latency | Ceiling |
|---|---|---|---|---|---|
| **Flubsound Signature** | Balanced everyday default for any genre | Bass +1.5 dB @ 60 Hz (adaptive), light tighten, de-mud 0.1, a touch of air, glue 0.15 | Boost 35 %, Punch 20, Width 15, Clarity 20 | Balanced | −1 dBTP |
| **Punchy Pop** | Radio-ready energy for pop / dance-pop | Bass +2 dB, tighten 0.2, mono bass < 90 Hz, −1 dB @ 250 Hz, glue 0.25 | Boost 40 %, Punch 35, Width 15, Clarity 20, Loudness 25 | Balanced | −1 dBTP |
| **Bass Head** | Deep, heavy but controlled low end (hip-hop, EDM) | Bass +6 dB @ 55 Hz, protection raised to −4 dBFS, harmonic bass 0.2 (cutoff 90 Hz), mono < 110 Hz, subsonic 25 Hz, −1.5 dB @ 250 Hz, EQ out −1.5 dB, glue 0.3 | Boost 45 %, Punch 25, Loudness 15 | Balanced | −1 dBTP |
| **Warm Vinyl** | Analogue record-player colour | Tape saturation (drive 2 dB, mix 0.7, 2× HQ), +1 dB @ 220 Hz, −1.5 dB high shelf @ 9 kHz, mono bass < 150 Hz (vinyl cut), attack −1 dB, width 0.9, clipper 0.35 | Boost 25 %, Warmth 35 | Quality | −1 dBTP |
| **Crystal Clarity** | Maximum detail without harshness | De-mud 0.15 + −1 dB @ 280 Hz, dynamic presence @ 3 kHz, air 0.15, +1 dB shelf @ 12 kHz, **7 kHz de-esser** (dyn band 3) | Boost 30 %, Clarity 40 (adds the internal de-harsh band), Punch/Width 10 | Balanced | −1 dBTP |
| **Wide Stage** | Bigger, open stereo image | Width 1.15 above 220 Hz, space 0.2, mono bass < 120 Hz, stricter mono safety (min. correlation 0.15), air 0.1 | Boost 30 %, Width 40, Clarity 15, Punch 10 | Balanced | −1 dBTP |
| **Late Night Low Volume** | Quiet listening without losing detail | **Auto Level → −20 LUFS**; compressor 2:1 @ −26 dB (20 ms / 250 ms auto) + **upward 6 dB below −40 dB**; loudness contour: adaptive bass +3 dB @ 80 Hz, +1.5 dB shelf @ 10 kHz; presence 0.2 @ 2.5 kHz | Boost 20 % | Balanced | −1 dBTP |
| **Audiophile Subtle** | Almost transparent | Bass +0.8 dB @ 40 Hz, subsonic 10 Hz, air 0.06, crossfeed 0.15, dynamic EQ off, **limiter only** (no clipper, no drive) | none | Quality | −1 dBTP |
| **Club Loud** | Loud and dense for parties / EDM | **−9 LUFS loudness cap** (AutoDrive only ever *reduces* drive), glue 0.4, bass +3 dB (protection −8 dBFS), tighten 0.2, mono < 100 Hz, subsonic 28 Hz | Boost 45 %, **Loudness 60** (≈ +6 dB governed drive), Punch 15, Width 10 | Balanced | −1 dBTP |
| **Podcast & Voice** | Clear, even speech | **Spectral gate** (−9 dB); low cut 80 Hz; −2 dB @ 250 Hz; compressor 2.5:1 @ −14 dB, +4 dB makeup; **Auto Level → −16 LUFS**; de-esser 6.5 kHz; presence 0.3 @ 2.8 kHz; de-mud 0.35; width 0.8; bass engine off | Boost 20 %, Clarity 20 | Quality | −1 dBTP |
| **Lo-Fi Chill** | Relaxed, dusty tape character | Tape saturation (drive 6 dB, mix 0.6), high cut 10 kHz (12 dB/oct), low cut 35 Hz, +1 dB @ 500 Hz, attack −2 / sustain +1 dB, width 0.85, space 0.2, clipper 0.3 | Boost 25 %, Warmth 40 | Quality | −1 dBTP |
| **Classical & Jazz Dynamic** | Full dynamic range for acoustic recordings | **No drive, no clipper, no compression**; slow limiter (250 ms); bass +1.2 dB @ 80 Hz; air 0.12; crossfeed 0.3 for hard-panned vintage jazz | none | Quality | −1 dBTP |

## Gaming (Gaming mode)

| Preset | Intent | Key settings (base values) | Boost / macros (Footsteps · Positional · Impact · Detail · Voice) | Latency | Ceiling |
|---|---|---|---|---|---|
| **Competitive FPS** | Tactical shooters: footsteps and direction first | **No broadband downward compression** (ratio 1:1, so the compressor only lifts), upward threshold −36 dB, 2 ms attack; −2 dB low shelf @ 100 Hz and subsonic 30 Hz (anti-masking); crossfeed 0 (forced in Gaming mode) | Boost 35 % · **80** · **55** · 10 · 30 · 35 | **Low Latency** | −1 dBTP |
| **Battle Royale** | Large maps, distant gunfire, squad comms | Gentle 1.5:1 @ −14 dB (3 ms / 180 ms) tames close explosions; upward −42 dB; −1 dB low shelf @ 100 Hz | Boost 40 % · 60 · 55 · 25 · 45 · 40 | **Low Latency** | −1 dBTP |
| **Cinematic Adventure** | Story / open-world / RPG: impact and score | Bass +2 dB @ 50 Hz, harmonics 0.1, width 1.2 above 200 Hz, space 0.15, virtualiser room 0.2, gentle compression 1.5:1 @ −12 dB (20 / 250 ms auto), maximizer glue 0.25 | Boost 45 % · 0 · 30 · **50** · 25 · **45** | Balanced | −1 dBTP |
| **Horror Detail** | Stealth and horror: the quietest sounds matter | Upward compression 2.5:1 below −38 dB (floor −70 dB, so noise is not lifted); light 1.3:1 @ −12 dB; drone lift +1.5 dB @ 45 Hz | Boost 35 % · 50 · 40 · 15 · **60** · 30 | Balanced | −1 dBTP |
| **Racing** | Engine body, tyre cues, spotter | Bass +2 dB @ 60 Hz, harmonics 0.15 (cutoff 100 Hz), tighten 0.2, de-mud 0.2, presence @ 2.5 kHz, 1.8:1 @ −16 dB auto release, width 1.1 | Boost 40 % · 0 · 40 · 35 · 30 · 35 | Balanced | −1 dBTP |
| **MOBA & Strategy** | Voice and announcer clarity, fatigue-free sessions | Dynamic cut @ 4.5 kHz (piercing spell / UI sounds), −1 dB shelf @ 9 kHz, 1.8:1 @ −16 dB (200 ms auto), width 1.1 | Boost 30 % · 0 · 30 · 15 · 20 · **60** | Balanced | −1 dBTP |
| **7.1 Headphone Surround** | Game set to 7.1 → binaural virtualiser | Virtual speakers: sides 90°, rears 150° (clear side/back separation), room 0.22, LFE −4 dB; 1.5:1 @ −16 dB. Width, space and crossfeed are locked by the binaural policy | Boost 30 % · 45 · 40 · 30 · 30 · 35 | Balanced | −1 dBTP |
| **Night Mode Gaming** | Late-night play at low volume | **Auto Level → −20 LUFS**; 3:1 @ −24 dB (3 ms / 250 ms auto), +6 dB makeup; **upward 6 dB below −38 dB** (floor −68 dB); −3 dB low shelf @ 90 Hz | **Boost 15 %** · 35 · 30 · 0 · 35 · 45 | Balanced | −1 dBTP |
| **Tournament Clean** | Leagues that restrict audio processing | **Every module off** except the true-peak limiter (limiter only, no clipper, no drive); virtualiser off (multichannel is downmixed) | none | **Low Latency** | −1 dBTP |

## Device (Music mode)

| Preset | Intent | Key settings (base values) | Boost / macros | Latency | Ceiling |
|---|---|---|---|---|---|
| **Laptop Speakers** | Fuller, wider built-in laptop / tablet speakers | **Small-speaker mode**: harmonic bass 0.4, speaker limit (cutoff) 150 Hz, character 0.6, subsonic 40 Hz; width 1.25 above 300 Hz; −1.5 dB @ 3.5 kHz; parallel compressor 1.6:1 @ −20 dB (mix 0.7, SC high-pass 150 Hz); glue 0.3 | Boost 40 %, Punch 15, Clarity 10, Loudness 35 | Balanced | −1 dBTP |
| **Earbuds** | In-ear and open earbuds | Bass +3 dB @ 60 Hz (protection −6 dBFS: it compensates for fit and seal), harmonics 0.12 (cutoff 70 Hz), tighten 0.15, **dynamic cut @ 6.5 kHz** (in-ear resonance), crossfeed 0.25, de-mud 0.15 | Boost 35 %, Punch 15 | Balanced | −1 dBTP |
| **Bluetooth Headphones** | Bluetooth A2DP headphones and earbuds | **Ceiling −2 dBTP** (the lossy codec overshoots), soft clipper (share 0.3, knee 0.7), air exciter kept low, −1 dB shelf @ 14 kHz, mono bass < 120 Hz, width 0.95, crossfeed 0.15, bass +2.5 dB @ 60 Hz | Boost 30 %, Punch 10 | Balanced | **−2 dBTP** |

### Engineering notes and deviations

* **Output protection, in every preset:**
  * The maximizer (true-peak limiter) stays on.
  * `output.gain` is never positive, because it is applied *after* the limiter.
  * The ceiling is −1 dBTP, or −2 dBTP for Bluetooth. Device profiles may lower it further (−3 dBTP for hands-free).
* **Gaming compressor settings are always explicit.** In Gaming mode, Boost ≥ 7 %, *Footsteps* and *Detail* all switch the compressor on. Every Gaming preset therefore sets the downward threshold, ratio and makeup on purpose. *Competitive FPS* uses ratio 1:1, so the compressor only lifts quiet sounds and gunfire is never compressed.
* **Bass protection is adaptive.** `bass.protect` caps the *predicted* low-frequency level, so the adaptive shelf gives way on bass-heavy masters instead of making the limiter pump. The default (−12 dBFS) is kept wherever the boost is a matter of taste. It is raised only where the boost is the point of the preset or compensates for the device: Bass Head −4, Earbuds −6, Club Loud / Bluetooth / Late Night −8.
* **Laptop Speakers subsonic is 40 Hz, not 60 Hz.** 60 Hz would suit the drivers, but the `bass.subsonic` range is 0–40 Hz, so 60 would be clamped to 40. With small-speaker mode on, everything below the 150 Hz speaker limit is high-passed away *after* the harmonics are generated. The drivers therefore never receive sub-150 Hz energy, and the 40 Hz subsonic only keeps rumble out of the harmonic generator.
* **Crossfeed 0 in Competitive FPS is not written in the file**, because it is the default. The chain also forces it to 0 in Gaming mode.
* **Auto Level** is used only where levelling is the point of the preset: *Late Night Low Volume*, *Podcast & Voice* and *Night Mode Gaming*. Elsewhere it would flatten the intended dynamics of music and games.

---

## Creating your own presets

1. **Start from the closest factory preset.** Load it, adjust it, and use *Save As*. Factory presets are read-only; user presets go to the user preset folder.
2. **Adjust from the top down.** Change Boost Intensity and the five macros first, then the module cards, and only then individual parameters. The macros are staged and governed, so they are the safest way to get more.
3. **Cut before you boost.** Keep headroom with `eq.output` or `input.gain` when you add large EQ or bass boosts. Never raise `output.gain` above 0 dB: it is applied after the limiter and would push peaks past the ceiling.
4. **Get loudness from the Loudness macro, or from a `max.target` with `max.autoDrive`.** A fixed `max.drive` bypasses the SafetyGovernor. If the governor meter drops below 100 %, or the limiter shows more than about 6 dB of sustained gain reduction, back off.
5. **Keep the ceiling at −1 dBTP or lower** (−2 dBTP on Bluetooth). Leave the maximizer on.
6. **Gaming:**
   * Keep footstep and positional enhancement in one place only. Turn off the headset's own "superhuman hearing", bass boost and virtual surround.
   * Use either the game's HRTF with a stereo output or 7.1 plus Flubsound's virtualiser, never both. See `docs/10-headset-compatibility.md`.
   * Prefer *Low Latency* for competitive play.
7. **Compare fairly.** Global bypass is loudness-matched by default (`bypass.matched`), so A/B comparisons judge tone and dynamics, not volume.

### File format

```json
{
  "format": "flubsound-preset",
  "version": 1,
  "name": "My FPS Tweak",
  "category": "Gaming",
  "author": "Me",
  "description": "What it is for and what it does, in two or three sentences.",
  "tags": ["fps", "footsteps"],
  "params": {
    "mode": "Gaming",
    "boost": 0.35,
    "macro.1": 0.8,
    "latency.profile": "Low Latency",
    "comp.ratio": 1,
    "eq.1.type": "Low Shelf",
    "eq.1.freq": 100,
    "eq.1.gain": -2
  }
}
```

* **Keys** are the stable string keys from `core/src/engine/Parameters.cpp`. `flubsound-cli params` lists every key with its range and default.
* **Values:**
  * Choices are written as their **label**, for example `"Gaming"`, `"Low Shelf"` or `"12 dB/oct"`.
  * Toggles are `true` / `false`.
  * Percentages are stored as 0–1.
  * Out-of-range numbers are clamped when loaded.
* **Keep files minimal:** store only values that differ from the defaults. Missing keys load as defaults, and unknown keys are ignored, so presets stay compatible in both directions between versions.
* **Mode:** Music presets leave `mode` out (Music is the default), and Gaming presets set `"mode": "Gaming"`.

### Adding a factory preset (contributors)

Name the file `presets/factory/<category>-<slug>.json` in lower case, where the category is `music`, `gaming` or `device`. Then run:

```bash
cmake --build build --target flub_tests && ./build/tests/flub_tests "Factory presets"
```

The test enforces all of the following:

* **Metadata:**
  * name, category, description and tags are present;
  * the author is `Flubsound`;
  * the category is Music, Gaming or Device;
  * Gaming presets are in Gaming mode.
* **Parameters:**
  * every key is known and appears only once;
  * choice labels are valid, toggles are booleans and numbers are in range;
  * no default values are stored.
* **Output protection:**
  * the ceiling is ≤ −1 dBTP (≤ −2 dBTP for Bluetooth);
  * the maximizer is on;
  * the output gain is ≤ 0 dB;
  * `max.drive` stays 0 (fixed drive is not governed).
* **Macro stacking:** with Boost Intensity and all five macros at 100 %, `bass.boost`, `bass.harmonics`, `max.drive` and `sat.drive` must not be pinned at the top of their range (the base value must leave the macros headroom).
* **Gaming policy:**
  * no stored crossfeed, width ≤ 1.25, space ≤ 0.2;
  * if the compressor is engaged, its ratio or threshold is set explicitly;
  * *Competitive* and *Tournament* presets use Low Latency, and the `low-latency` tag goes with the Low Latency profile in every category.
* **Render:** a hot 4 s drum, bass and pad programme is rendered through the chain. The chain uses 8 channels for the 7.1 preset and is prepared after loading, so the latency profile is honoured. The test checks that:
  * the output is finite;
  * the sample peak stays at or below the ceiling;
  * the true peak is within 0.15 dB of the ceiling;
  * no safety clips occur and the output is not silent.
* **Stress render:** the same programme, with Boost Intensity and all macros at 100 %, for the first 2 s (before the SafetyGovernor reacts). The output must be finite, sample peaks must stay at or below the ceiling and the safety clamp must not engage. The true-peak tolerance here is 0.5 dB, because of a known limiter issue in the core that is tracked separately. It will be tightened to 0.15 dB once that is fixed.
* **Cross-references:** every preset that a device profile suggests exists.
