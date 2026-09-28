# Flubsound factory presets

This folder holds the factory preset library: 25 presets in three categories (13 Music, 9 Gaming, 3 Device). It has two parts:

| Folder | Contents |
|---|---|
| `factory/` | `<category>-<slug>.json` preset files, top level only (no sub-folders). The desktop app embeds them as BinaryData, `flubsound-cli` finds them at run time, and `cmake --install` copies them to `share/flubsound/presets/factory`. |
| `devices/device-profiles.json` | Headset and output-device profiles. They add device-aware advice and ceiling caps, and suggest presets by name (for example *Competitive FPS*, *Flubsound Signature* or *Bluetooth Headphones*). |

`tests/test_factory_presets.cpp` checks every file on every build. It confirms the metadata, that every key and choice label is known, that only non-default values are stored and that the ceilings are right. It also renders each preset through the full `ProcessingChain` and checks the output against its ceiling. Details are in the last section.

---

## How a preset works

A preset stores **base values** only. Two further layers are added on top of them in real time (`core/src/engine/MacroMap.cpp`):

* **Boost Intensity** is staged, so each part arrives at a different point:
  * clarity, width and detail come in first;
  * bass and impact come next;
  * maximizer drive and saturation come in last (drive starts at 30 %, saturation drive at 60 %; in Music mode the multiband glue joins from 40 %).
* **The five mode macros:**
  * Music mode: *Punch, Width, Clarity, Loudness, Warmth*.
  * Gaming mode: *Footsteps, Positional, Impact, Detail, Voice & Score*.
  * Several macros also drive internal dynamic-EQ bands, such as the footstep cue enhancer (3.2 kHz and 260 Hz: it lifts what rises out of the ambience, not the ambience itself, steady sounds or the loudest events, and its 3.2 kHz band is off on 32 kHz-and-below Bluetooth hands-free links), the voice band, de-harsh and de-boom. Explosion anti-masking no longer follows *Footsteps* (docs/11 E20): the gaming presets that tame loud low end store it as dynamic-EQ user band 1 (`dyneq.0.*`, a −22 dBFS 3:1 low shelf at 90 Hz).

Every factory preset sets Boost and the macros to a sensible *starting point*, so the user can go further from there. Anything that adds loudness or drive through Boost or a macro (maximizer drive, saturation drive, bass boost, harmonic bass) is **governed**: the SafetyGovernor scales it back when the limiter or clipper is working too hard. For this reason the factory presets get their loudness from the governed controls (Boost, Loudness), not from a fixed `max.drive`.

The maximizer's 3-band **glue** only runs while it is *armed*: the preset sets `max.glue` above 0, or a macro that can raise it (Boost Intensity or Loudness, Music mode only) is above zero. A preset without glue and with those controls at zero keeps the splitter out of the signal path entirely.

**Latency profiles** (48 kHz, algorithmic, with the current modules). The profile is chosen in Settings and is application state, like Bypass: a preset never sets it, so loading or stepping presets never changes the latency or re-prepares the engine (docs/11 E40). The *Suggested latency* columns below list the profile each preset is made for: the one it names in its `"suggestedLatencyProfile"` label, or Balanced (the default) when it names none.

| Profile | Latency | What it enables |
|---|---|---|
| **Quality** | ~28 ms | Spectral noise gate in the chain, HQ oversampling, longest look-ahead. Still well inside lip-sync tolerance for video, but not for competitive play |
| **Balanced** | ~4 ms | Default. Safe for video lip-sync |
| **Low Latency** | ~2.1 ms | Short look-ahead and oversampling, for competitive play |

These are per-strip chain figures. The desktop app adds 1.4 ms for its master limiter after the strip sum (see `docs/01-architecture.md` §5). The spectral noise gate (used by *Podcast & Voice*) runs only in Quality; in the other profiles the rest of such a preset still applies.

**Fresh strips** (the desktop app, a strip with no saved state: [docs/11 E36](../docs/11-enhancement-report.md#e36)) start from a factory preset instead of the parameter defaults at Boost 0: *Flubsound Signature* on Music and System, *Voice Chat* on Chat, and on Game *First Run – Game*, which is *Competitive FPS* with Boost 20 %, Footsteps 30 % and Detail 15 % (shown as a modified *Competitive FPS*). As shipped, *Competitive FPS* lifts −50 / −60 dBFS pink beds by +4.9 / +9.7 LU; the capped version by +2.7 / +2.6 LU, with step/bed contrast changes of +0.06 to +0.49 dB on the E59 burst scenes. None of them sets a latency profile, and saved strip state is never overwritten.

---

## Music (Music mode)

| Preset | Intent | Key settings (base values) | Boost / macros | Suggested latency | Ceiling |
|---|---|---|---|---|---|
| **Flubsound Signature** | Balanced everyday default for any genre | Bass +1.5 dB @ 60 Hz (adaptive), light tighten, de-mud 0.1, a touch of air, glue 0.15 | Boost 35 %, Punch 20, Width 15, Clarity 20 | Balanced | −1 dBTP |
| **Punchy Pop** | Radio-ready energy for pop / dance-pop | Bass +2 dB, tighten 0.2, mono bass < 90 Hz, −1 dB @ 250 Hz, glue 0.25 | Boost 40 %, Punch 35, Width 15, Clarity 20, Loudness 25 | Balanced | −1 dBTP |
| **Bass Head** | Deep, heavy but controlled low end (hip-hop, EDM) | Bass +6 dB @ 55 Hz, protection raised to −4 dBFS, harmonic bass 0.2 (cutoff 90 Hz), mono < 110 Hz, subsonic 25 Hz, −1.5 dB @ 250 Hz, EQ out −1.5 dB, glue 0.3 | Boost 45 %, Punch 25, Loudness 15 | Balanced | −1 dBTP |
| **Warm Vinyl** | Analogue record-player colour | Tape saturation (drive 2 dB, mix 0.7, 2× HQ), +1 dB @ 220 Hz, −1.5 dB high shelf @ 9 kHz, mono bass < 150 Hz (vinyl cut), attack −1 dB, width 0.9, clipper 0.35 | Boost 25 %, Warmth 35 | Quality | −1 dBTP |
| **Crystal Clarity** | Maximum detail without harshness | De-mud 0.15 + −1 dB @ 280 Hz, dynamic presence @ 3 kHz, air 0.15, +1 dB shelf @ 12 kHz, **7 kHz de-esser** (dyn band 3) | Boost 30 %, Clarity 40 (adds the internal de-harsh band), Punch/Width 10 | Balanced | −1 dBTP |
| **Wide Stage** | Bigger, open stereo image | Width 1.15 above 220 Hz, space 0.2, mono bass < 120 Hz, stricter mono safety (min. correlation 0.15), air 0.1 | Boost 30 %, Width 40, Clarity 15, Punch 10 | Balanced | −1 dBTP |
| **Late Night Low Volume** | Quiet listening without losing detail | **Auto Level → −20 LUFS**; compressor 2:1 @ −26 dB (20 ms / 250 ms auto) + **upward 6 dB below −40 dB**; fixed low-volume contour (it does not follow the volume setting): bass +3 dB @ 80 Hz (protection −8 dBFS), +1.5 dB shelf @ 10 kHz; presence 0.2 @ 2.5 kHz | Boost 20 % | Balanced | −1 dBTP |
| **Audiophile Subtle** | Almost transparent | Bass +0.8 dB @ 40 Hz, subsonic 10 Hz, air 0.06, crossfeed 0.15, dynamic EQ off, **limiter only** (no clipper, no drive) | none | Quality | −1 dBTP |
| **Club Loud** | Loud and dense for parties / EDM | **−9 LUFS loudness cap** (AutoDrive only ever *reduces* drive), glue 0.4, bass +3 dB (protection −8 dBFS), tighten 0.2, mono < 100 Hz, subsonic 28 Hz | Boost 45 %, **Loudness 60** (≈ +6 dB governed drive), Punch 15, Width 10 | Balanced | −1 dBTP |
| **Podcast & Voice** | Clear, even speech | **Spectral gate** (−9 dB); low cut 80 Hz; −2 dB @ 250 Hz; compressor 2.5:1 @ −14 dB, +4 dB makeup; **Auto Level → −16 LUFS**; de-esser 6.5 kHz; presence 0.3 @ 2.8 kHz; de-mud 0.35; width 0.8; bass engine off | Boost 20 %, Clarity 20 | Quality | −1 dBTP |
| **Lo-Fi Chill** | Relaxed, dusty tape character | Tape saturation (drive 6 dB, mix 0.6), high cut 10 kHz (12 dB/oct), low cut 35 Hz, +1 dB @ 500 Hz, attack −2 / sustain +1 dB, width 0.85, space 0.2, clipper 0.3 | Boost 25 %, Warmth 40 | Quality | −1 dBTP |
| **Voice Chat** | The Chat strip's default: even, intelligible voice chat (Discord, Teams, in-game voice) | Low cut 110 Hz; de-mud 0.3; presence 0.3 @ 2.8 kHz; de-esser 6.5 kHz; speech leveller: **Auto Level → −19 LUFS**, compressor 3:1 @ −30 dB (5 ms / 150 ms, +16 dB makeup) and upward 3:1 below −42 dB (up to +12 dB); bass engine off; **no loudness maximizing**: the maximizer is only its true-peak limiter (no drive, clipper share 0, no glue) | Boost 15 % (below the 25 % where Boost starts driving the maximizer) | Balanced | −1 dBTP |
| **Classical & Jazz Dynamic** | Full dynamic range for acoustic recordings | **No drive, no clipper, no compression**; slow limiter (250 ms); bass +1.2 dB @ 80 Hz; air 0.12; crossfeed 0.3 for hard-panned vintage jazz | none | Quality | −1 dBTP |

## Gaming (Gaming mode)

| Preset | Intent | Key settings (base values) | Boost / macros (Footsteps · Positional · Impact · Detail · Voice) | Suggested latency | Ceiling |
|---|---|---|---|---|---|
| **Competitive FPS** | Tactical shooters: footsteps and direction first | **No broadband downward compression** (ratio 1:1, so the compressor only lifts), upward threshold −36 dB, 2 ms attack; −2 dB low shelf @ 100 Hz, subsonic 30 Hz and a 4.8 dB dynamic anti-masking shelf @ 90 Hz; crossfeed 0 (forced in Gaming mode) | Boost 35 % · **80** · **55** · 10 · 30 · 20 | **Low Latency** | −1 dBTP |
| **Battle Royale** | Large maps, distant gunfire, squad comms | Gentle 1.5:1 @ −14 dB (3 ms / 180 ms, no make-up) tames close explosions; upward −42 dB; −1 dB low shelf @ 100 Hz and a 3.6 dB dynamic anti-masking shelf @ 90 Hz | Boost 35 % · **75** · 55 · 25 · 45 · 10 | **Low Latency** | −1 dBTP |
| **Cinematic Adventure** | Story / open-world / RPG: impact and score | Bass +2 dB @ 50 Hz, harmonics 0.1, width 1.2 above 200 Hz, space 0.15, virtualiser room 0.2, gentle compression 1.5:1 @ −12 dB (20 / 250 ms auto), maximizer glue 0.25 | Boost 45 % · 0 · 30 · **50** · 25 · **45** | Balanced | −1 dBTP |
| **Horror Detail** | Stealth and horror: the quietest sounds matter | Upward compression 2.5:1 below −38 dB (floor −70 dB, so noise is not lifted); light 1.3:1 @ −12 dB; drone lift +1.5 dB @ 45 Hz; 3 dB dynamic anti-masking shelf @ 90 Hz | Boost 35 % · 50 · 40 · 15 · **60** · 30 | Balanced | −1 dBTP |
| **Racing** | Engine body, tyre cues, spotter | Bass +2 dB @ 60 Hz, harmonics 0.15 (cutoff 100 Hz), tighten 0.2, de-mud 0.2, presence @ 2.5 kHz, 1.8:1 @ −16 dB auto release, width 1.1 | Boost 40 % · 0 · 40 · 35 · 30 · 35 | Balanced | −1 dBTP |
| **MOBA & Strategy** | Voice and announcer clarity, fatigue-free sessions | Dynamic cut @ 4.5 kHz (piercing spell / UI sounds), −1 dB shelf @ 9 kHz, 1.8:1 @ −16 dB (200 ms auto), width 1.1 | Boost 30 % · 0 · 30 · 15 · 20 · **60** | Balanced | −1 dBTP |
| **7.1 Headphone Surround** | Game set to 7.1 → binaural virtualiser | Virtual speakers: sides 90°, rears 150° (clear side/back separation), room 0.22, LFE −4 dB; 1.5:1 @ −16 dB; 2.7 dB dynamic anti-masking shelf @ 90 Hz. Width, space and crossfeed are locked by the binaural policy | Boost 30 % · 45 · 40 · 30 · 30 · 35 | Balanced | −1 dBTP |
| **Night Mode Gaming** | Late-night play at low volume | **Auto Level → −20 LUFS**; 3:1 @ −24 dB (3 ms / 250 ms auto), +6 dB makeup; **upward 6 dB below −38 dB** (floor −68 dB); −3 dB low shelf @ 90 Hz and a 2.1 dB dynamic anti-masking shelf | **Boost 15 %** · 35 · 30 · 0 · 35 · 45 | Balanced | −1 dBTP |
| **Tournament Clean** | Leagues that restrict audio processing | **Every module off** except the true-peak limiter (limiter only, no clipper, no drive): it keeps digital peaks under −1 dBTP and is not hearing protection; virtualiser off (multichannel is downmixed) | none | **Low Latency** | −1 dBTP |

## Device (Music mode)

| Preset | Intent | Key settings (base values) | Boost / macros | Suggested latency | Ceiling |
|---|---|---|---|---|---|
| **Laptop Speakers** | Fuller, wider built-in laptop / tablet speakers | **Small-speaker mode**: harmonic bass 0.4, speaker limit (cutoff) 150 Hz, character 0.6, subsonic at the 20 Hz default; width 1.25 above 300 Hz; −1.5 dB @ 3.5 kHz; parallel compressor 1.6:1 @ −20 dB (mix 0.7, SC high-pass 150 Hz); glue 0.3 | Boost 40 %, Punch 15, Clarity 10, Loudness 35 | Balanced | −1 dBTP |
| **Earbuds** | In-ear and open earbuds | Bass +3 dB @ 60 Hz (protection −6 dBFS: it compensates for fit and seal), harmonics 0.12 (cutoff 70 Hz), tighten 0.15, **dynamic cut @ 6.5 kHz** (in-ear resonance), crossfeed 0.25, de-mud 0.15 | Boost 35 %, Punch 15 | Balanced | −1 dBTP |
| **Bluetooth Headphones** | Bluetooth A2DP headphones and earbuds | **Ceiling −2 dBTP** (the lossy codec overshoots), soft clipper (share 0.3, knee 0.7), air exciter kept low, −1 dB shelf @ 14 kHz, mono bass < 120 Hz, width 0.95, crossfeed 0.15, bass +2.5 dB @ 60 Hz | Boost 30 %, Punch 10 | Balanced | **−2 dBTP** |

### Engineering notes and deviations

* **Output protection, in every preset:**
  * The maximizer (true-peak limiter) stays on.
  * `output.gain` is never positive: it is applied *after* the limiter, and its range ends at 0 dB.
  * The ceiling is −1 dBTP, or −2 dBTP for Bluetooth. Device profiles may lower it further (−3 dBTP for hands-free).
* **Gaming compressor settings are always explicit.** In Gaming mode *Detail* switches the compressor on (Boost and *Footsteps* no longer do since docs/11 E19: a broadband upward compressor lifts the ambience with the cues). If only it switched it on and `comp.ratio` is left at its default, the engine runs it at 1:1 (upward only), so quiet sounds come up and gunfire and explosions are not compressed by it. In Gaming the upward floor follows the programme's background (docs/11 E19): quiet sounds that rise out of the ambience come up, the steady ambience itself does not. Every Gaming preset that engages the compressor still sets the ratio on purpose: *Competitive FPS* uses 1:1, the others a gentle 1.3:1 to 3:1 with their own threshold and makeup.
* **Bass protection is adaptive.** `bass.protect` caps the *predicted* low-frequency level, so the adaptive shelf gives way on bass-heavy masters instead of making the limiter pump. The default (−12 dBFS) is kept wherever the boost is a matter of taste. It is raised only where the boost is the point of the preset or compensates for the device: Bass Head −4, Earbuds −6, Club Loud / Bluetooth / Late Night −8.
* **Laptop Speakers keeps the 20 Hz default subsonic.** With small-speaker mode on, everything below the 150 Hz speaker limit is high-passed away *after* the harmonics are generated, so the drivers never receive sub-150 Hz energy whatever the subsonic is. The subsonic runs *before* the harmonic generator, though, so it decides which fundamentals the harmonics are made from. It used to be 40 Hz, which removed most of the lowest octave the harmonics exist to restore: for a 30 Hz tone the audible-band (≥ 120 Hz) energy was −23.8 dB, against −14.8 dB at 20 Hz (−14.6 dB with the filter off; docs/11 E03). 20 Hz still keeps DC and infrasonic rumble out of the generator. The test suite requires `bass.subsonic` ≤ 20 Hz in every preset that replaces the fundamental.
* **Crossfeed 0 in Competitive FPS is not written in the file**, because it is the default. The chain also forces it to 0 in Gaming mode.
* **Auto Level** is used only where levelling is the point of the preset: *Late Night Low Volume*, *Podcast & Voice* and *Night Mode Gaming*. Elsewhere it would flatten the intended dynamics of music and games. It lifts a quiet source by at most 6 dB, and holds its gain through a loud event (a 400 ms loudness more than 8 LU above its 3 s measure) for up to 5 s, so an explosion does not leave the ambience after it several dB down.

---

## Creating your own presets

1. **Start from the closest factory preset.** Load it, adjust it, and use *Save As*. Factory presets are read-only; user presets go to the user preset folder.
2. **Adjust from the top down.** Change Boost Intensity and the five macros first, then the module cards, and only then individual parameters. The macros are staged and governed, so they are the safest way to get more.
3. **Cut before you boost.** Keep headroom with `eq.output` or `input.gain` when you add large EQ or bass boosts. `output.gain` sits after the limiter and can only attenuate (−24 … 0 dB).
4. **Get loudness from Boost Intensity and the Loudness macro** (both governed). `max.autoDrive` with `max.target` only *caps* loudness: AutoDrive can reduce the requested drive, never add to it. A fixed `max.drive` bypasses the SafetyGovernor. If the Boost panel's badge changes from "Safety governor OK" to "Safety governor: N% applied", or the limiter shows more than about 6 dB of sustained gain reduction, back off.
5. **Keep the ceiling at −1 dBTP or lower** (−2 dBTP on Bluetooth). Leave the maximizer on.
6. **Gaming:**
   * Keep footstep and positional enhancement in one place only. Turn off the headset's own "superhuman hearing", bass boost and virtual surround.
   * Use either the game's HRTF with a stereo output or 7.1 plus Flubsound's virtualiser, never both. See `docs/10-headset-compatibility.md`. A game that fills only FL/FR of the Game strip's 8 channels is detected and folded as plain stereo after 2 s (`virt.input` Auto; Force Surround / Force Stereo override it). For a game that renders its own HRTF, set `virt.ownHrtf`: it takes the virtualiser, width, focus, crossfeed and space out.
   * `virt.lfe` is the LFE level relative to one main channel, with the virtualiser on or off (default +6 dB; the in-band convention is +10 dB). `virt.lfeFold: false` drops the LFE.
   * Prefer the *Low Latency* profile (Settings) for competitive play, and set `"suggestedLatencyProfile": "Low Latency"` in competitive presets.
7. **Compare fairly.** Global bypass is loudness-matched by default (`bypass.matched`): from the first press of Bypass until it has been off for 10 s, the louder side (usually the processed one) is turned down to the other, so bypass comparisons judge tone and dynamics, not volume. Bank A/B flips are not matched yet.

### File format

```json
{
  "format": "flubsound-preset",
  "version": 2,
  "uuid": "0b7a4c1e-9d52-4f0e-8a31-5c2d7e6f9a10",
  "contentHash": "3f1d0c9e7a52b684",
  "name": "My FPS Tweak",
  "category": "Gaming",
  "author": "Me",
  "description": "What it is for and what it does, in two or three sentences.",
  "tags": ["fps", "footsteps"],
  "suggestedLatencyProfile": "Low Latency",
  "params": {
    "mode": "Gaming",
    "boost": 0.35,
    "macro.1": 0.8,
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
  * Out-of-range numbers are clamped when loaded, with a warning.
* **Keep files minimal:** store only values that differ from the defaults. Missing keys load as defaults, and unknown keys are ignored, so presets stay compatible in both directions between versions.
* **Warnings:** everything the reader ignores or changes is reported, one sentence each: unknown keys with the closest known key (`unknown parameter "bost" ignored (did you mean "boost"?)`), clamped values, unknown choice labels, values of the wrong type, an invalid `uuid` and a newer schema minor. `flubsound-cli` prints them as `warning: preset: ...` on stderr (also with `--quiet`) and lists them in `--json` `render.notes`; the plug-in's *Import* shows them.
* **Schema version** (`major.minor`, written as a number such as `2` or `2.1`, also read from a string): Flubsound writes `"version": 2` and reads every major up to 2. A newer *minor* of a known major loads, with a warning (a minor only adds optional fields and keys). A newer *major* is refused with a message ("saved by a newer Flubsound version"); the file is never changed. An older major is migrated in memory, one registry step per major (`preset::migrations()` in `core/src/io/PresetIO.cpp`); only an explicit save writes the current version. A missing key means the default *of the file's version*: version 1 (and a file without `"version"`) was written sparse against the version-1 defaults, so the 1 → 2 step fills its missing keys from the frozen version-1 table, and a default changed later never re-voices it. The only difference so far is `virt.lfe`: 0 dB in version 1, +6 dB in version 2. The factory presets are still version 1 (see the checks below), so they keep 0 dB unless they set `virt.lfe`. Changing any parameter default fails `tests/test_presets_golden.cpp` until the major is bumped with a migration that fills the old value.
* **`uuid`** (optional, RFC 4122, stored lower case) identifies a preset across renames: auto-profile rules, hotkeys and content packs should refer to presets by it (`preset::findByUuid`). Give every new preset its own (`python3 -c 'import uuid; print(uuid.uuid4())'`) and never change it; the plug-in's *Export* writes a new one.
* **`contentHash`** is written on save: 16 hex digits of a hash of every sound parameter's value (app state excluded). It identifies the sound, not the file, and is not a checksum; a mismatch on load only means the file was edited or the parameter table grew since.
* **Mode:** Music presets leave `mode` out (Music is the default), and Gaming presets set `"mode": "Gaming"`.
* **App state is not part of a preset:** `bypass`, `bypass.matched` and `latency.profile` belong to the application. Loading a preset never changes them. Name the profile a preset is made for with the optional top-level `"suggestedLatencyProfile"` label (`"Quality"`, `"Balanced"` or `"Low Latency"`); it is metadata, never applied on load. A `latency.profile` in `params` (older files) is read as that suggestion.

### Adding a factory preset (contributors)

Name the file `presets/factory/<category>-<slug>.json` in lower case, where the category is `music`, `gaming` or `device`. Then run:

```bash
cmake --build build --target flub_tests && ./build/tests/flub_tests "Factory presets"
```

The test enforces all of the following:

* **Library:**
  * the file name is `<category>-<slug>.json` in lower case, and `presets/factory` has no sub-folders;
  * preset names are unique, all three categories exist, and the presets that code and docs refer to by name exist.
* **Metadata:**
  * `"format": "flubsound-preset"`, `"version": 1`;
  * a lower-case `uuid`, unique among the factory presets, and no `fromJson` warning;
  * name, category, description and tags are present;
  * the author is `Flubsound`;
  * the category is Music, Gaming or Device;
  * Gaming presets are in Gaming mode and Music presets in Music mode (the Device presets use Music mode too).
* **Parameters:**
  * every key is known and appears only once;
  * choice labels are valid, toggles are booleans and numbers are in range;
  * no default values are stored.
* **Output protection:**
  * the ceiling is ≤ −1 dBTP (≤ −2 dBTP for Bluetooth);
  * the maximizer is on;
  * the output gain is ≤ 0 dB;
  * no app-state key (`bypass`, `bypass.matched`, `latency.profile`) is stored, and `suggestedLatencyProfile`, if present, is a valid profile label;
  * `max.drive` stays 0 (fixed drive is not governed).
* **Macro stacking:** with Boost Intensity and all five macros at 100 %, `bass.boost`, `bass.harmonics`, `max.drive` and `sat.drive` must not be pinned at the top of their range (the base value must leave the macros headroom).
* **Gaming policy:**
  * no stored crossfeed, width ≤ 1.25, space ≤ 0.2;
  * if the compressor is engaged, its ratio or threshold is set explicitly;
  * *Competitive* and *Tournament* presets suggest Low Latency, and the `low-latency` tag goes with a suggested Low Latency profile in every category.
* **Small-speaker mode:** a preset with `bass.replaceFundamental` keeps `bass.subsonic` ≤ 20 Hz (the subsonic runs before the harmonic generator).
* **Loading:** on a strip in each of the three profiles, with bypass on and matched bypass off, loading the preset (through `applyPresetToStore` and through `applyToStore`) leaves all three values unchanged and needs no re-prepare.
* **Render:** a hot 4 s drum, bass and pad programme (48 kHz, 512-sample blocks) is rendered through the chain in Balanced, in Low Latency (the shortest limiter look-ahead) and, when the preset suggests it, in Quality: the user, not the preset, picks the profile. The chain uses 8 channels for the 7.1 preset. The test checks that:
  * the chain latency is within the profile's bound (40 / 5 / 2.5 ms for Quality / Balanced / Low Latency);
  * the output is finite, and channels above the stereo pair are cleared;
  * the sample peak stays at or below the ceiling;
  * the true peak (4× meter) is at most 0.15 dB above the ceiling;
  * no safety clips occur and the output is not silent.
* **Stress render:** the same programme in the same profiles, with Boost Intensity and all macros at 100 %, for the first 2 s (before the SafetyGovernor reacts). The output must be finite, sample peaks must stay at or below the ceiling, the true peak within the same 0.15 dB, and the safety clamp must not engage.
* **Cross-references:** every preset that a device profile suggests exists.
* **Golden** (`./build/tests/flub_tests Golden`, `tests/golden/`): every parameter default matches `parameter-defaults.json`, and every factory preset keeps its uuid and `contentHash` (`factory-presets.json`). With `FLUB_GOLDEN_REFERENCE=1` on the reference platform (Linux x86-64, gcc Release; CI's core job, gcc leg) every factory preset is also rendered (3 s of pink noise and 55 Hz kicks, Balanced) and its integrated LUFS and 1/3-octave band levels must stay within 0.05 dB of the recorded golden render. For an intended change, re-record with `FLUB_GOLDEN_UPDATE=1 ./build/tests/flub_tests Golden` on the reference platform and name the change.

### Changing how a preset sounds (contributors)

The checks above prove that a preset is safe, not how it sounds. Any change that alters the sound of a factory preset, a macro or a mode band (a preset value, a `MacroMap` row, `configureModeBands`) is measured before and after with the all-preset render diff ([docs/11 E59](../docs/11-enhancement-report.md#e59)), and its output is attached to the change:

```bash
python3 tools/scripts/preset-render-diff.py --cli build/tools/flubsound-cli/flubsound-cli
```

It renders all 25 presets, each in the latency profile it suggests, on five pinned programmes (a drum programme, a quiet game bed with footsteps, a 2 kHz tone under kicks, an ambush scene, a 7.1 bed with an LFE tone) and compares the output loudness, octave bands, 1 s level profile, pumping index and the CLI's render statistics with `tests/golden/preset-render-baseline.json`, listing every value that moved by more than 0.1 dB. The committed baseline comes from a gcc Release build on Linux x86-64; on another compiler, record your own on the base commit first (`--baseline /tmp/before.json --update`). Re-record the committed baseline with `--update` in the change that moves the sound on purpose.

The known sound-quality gaps that the planned retunes address (the Night Mode post-event hole, the Punch kick onset, pumping, 60 Hz THD+N, the 7.1 LFE, the 3.2 kHz lift at Bluetooth hands-free rates) are pinned by the `KnownGap:` tests in `tests/test_known_gaps.cpp` at today's values: `./build/tests/flub_tests KnownGap` prints every metric. The gaming presets are also judged on the docs/11 E60 scenes (a footstep burst scene, quiet → combat and an ambush, at −14 / −24 / −40 LUFS) in `tests/test_scenes.cpp`, which pins nine metrics per preset: `./build/tests/flub_tests Scenes:`. A change that closes a gap updates its expectation to the target named there, as the footstep cue enhancer did for short bursts out of silence and step/bed contrast (docs/11 E19), the Laptop Speakers subsonic fix for the 30 Hz audible band (`KnownGap closed:`, now checked against its ≥ −15.8 dB target) and the 3 dB positional-focus cap did for the focus ILD (≤ 3 dB added at 3 kHz).
