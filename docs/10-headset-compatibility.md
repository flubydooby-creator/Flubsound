# 10 — Headset Compatibility (incl. all Turtle Beach headsets)

> **Short answer.** Flubsound is compatible with every Turtle Beach headset that is connected to a Windows, macOS or Linux computer: wired 3.5 mm, USB, 2.4 GHz wireless (USB transmitter / dongle), Xbox Wireless adapter and Bluetooth.
>
> Flubsound processes audio *before* the operating system delivers it to the output device. Every one of those connections presents itself to the OS as a **standard audio device class**: an analog output, a USB Audio Class device or a Bluetooth audio profile. Flubsound needs no model-specific driver or SDK. On top of that, it recognises Turtle Beach product families and applies connection-specific safety settings and advice (§3).
>
> Flubsound is not affiliated with or endorsed by Turtle Beach Corporation. Product and family names are used only to identify compatible hardware.

---

## 1. How each connection type works with Flubsound

| Connection | How the OS sees the headset | Flubsound behaviour | Notes |
|---|---|---|---|
| **3.5 mm analog** (e.g. wired Recon, Elite Pro, many Stealth models in wired mode) | As the computer's own analog output (onboard codec, or a USB sound card / amplifier) | Select that output device; everything works unchanged | If the headset plugs into a controller or amplifier, that device is the output. |
| **USB (wired)** | USB Audio Class device (usually 48 kHz, 16- or 24-bit, stereo; some expose 7.1) | The engine opens the device at its native rate and processes in 32-bit float; a 7.1-capable output simply receives stereo | A headset that exposes 7.1 with its own virtual surround should have that turned off while Flubsound's virtualiser is in use (§4). |
| **2.4 GHz wireless via USB transmitter** (e.g. wireless Stealth and Atlas models) | Also a USB Audio Class device (the transmitter is the audio device) | Same as wired USB. The engine keeps running when the headset itself powers off, because the transmitter stays connected. | Wireless adds its own small, fixed latency that Flubsound cannot change. This is the recommended connection for games. |
| **Xbox Wireless adapter on PC** (Xbox models, including Turtle Beach) | A normal output endpoint | Works unchanged | Turn off Windows Sonic / Dolby Atmos for Headphones on that endpoint if you use Flubsound's virtualiser. |
| **Bluetooth, stereo (A2DP)** | Bluetooth output (lossy codec: SBC/AAC/…) | Output ceiling is capped at **−2 dBTP**, because lossy codecs overshoot peaks; the *Bluetooth Headphones* preset is suggested | Bluetooth adds roughly 100–300 ms of codec latency, independent of Flubsound. Prefer the USB transmitter or a cable for competitive play. |
| **Bluetooth hands-free (HFP/HSP)**, active while the headset microphone is open | Mono, narrowband output (8 or 16 kHz) | Detected from the endpoint name or format. Ceiling capped at **−3 dBTP**, the air exciter is disabled below 42 kHz to prevent aliasing, and the whole chain runs correctly at 8–16 kHz (tested) | Audio quality is limited by the Bluetooth profile itself. Use the stereo endpoint for game/music audio and a separate mic path if possible. |
| **Console only** (headset plugged into an Xbox / PlayStation / Switch with no PC in the path) | — | **Not applicable.** Flubsound runs on computers and cannot process audio that never passes through one. | Routing console audio through a PC (e.g. a capture card with monitoring) makes it processable. |

### Sample rates and formats

The engine follows whatever rate the device's audio format uses. The chain is tested at **8, 16, 22.05, 24, 32, 44.1, 48, 88.2, 96, 176.4 and 192 kHz**, in Music and Gaming mode, with Boost Intensity and every macro at 100 % and the compressor and saturator on:
- output is finite and not silent,
- the sample peak stays at or below the −1 dBTP ceiling,
- the chain's algorithmic latency stays below 6 ms at ≥ 22.05 kHz and below 12 ms at the Bluetooth hands-free rates (the FIR oversampling delays are fixed in samples).

The test is "Chain: runs at every sample rate a headset may use" in `tests/test_engine.cpp`. Bit depth is handled by the OS or the device layer; internal processing is always 32-bit float.

---

## 2. Turtle Beach family matrix

"✓" means the connection is a standard device class that Flubsound supports. The right-hand columns show what the device profile adds. Every family entry is currently `labVerified: false`: it is based on the family's documented connection types and features, and confirming each model in the device lab is part of the QA plan (§5).

| Family (profile id) | Wired 3.5 mm | USB / 2.4 GHz transmitter | Bluetooth | On-board / companion-app DSP to keep neutral | Profile advice |
|---|---|---|---|---|---|
| **Stealth** series (`turtle-beach-stealth`), e.g. Stealth Pro, 700, 600, 500, 300 | ✓ (wired models / modes) | ✓ | ✓ where the model has it | Superhuman Hearing, EQ presets, virtual surround | Ceiling cap by connection; "no double footstep/bass enhancement"; Game/Chat routing advice; suggested presets *Competitive FPS* / *Flubsound Signature* |
| **Recon** series (`turtle-beach-recon`), e.g. Recon 50, 70, 200, 500, Spark | ✓ | ✓ (amplified / USB variants) | — | Amplifier bass boost (where present) | Select the jack / amplifier output device |
| **Atlas / Elite Atlas** (`turtle-beach-atlas`) | ✓ | ✓ (wireless models) | — | PC companion software: 3D / virtual surround, EQ | Turn off the software's virtual surround when using Flubsound's virtualiser |
| **Elite Pro** (`turtle-beach-elite-pro`) | ✓ | ✓ via external USB amplifier / DAC | — | Amplifier / controller sound modes | The amplifier is the output device; keep its modes neutral |
| **ROCCAT** headsets (Turtle Beach brand) (`turtle-beach-roccat`), e.g. Syn, Elo | ✓ | ✓ | where available | 7.1 / 3D audio and EQ in software | Keep them neutral |
| **Xbox Wireless** models on PC (`xbox-wireless-headset`) | — | ✓ (Xbox Wireless adapter) | — | Windows Sonic / Dolby Atmos for Headphones | Turn off OS spatial sound with Flubsound's virtualiser |
| Legacy **Ear Force** and any other Turtle Beach device (`turtle-beach-generic`) | ✓ | ✓ | ✓ | Any on-board enhancement | Generic Turtle Beach advice |

Families are matched on whole words in the endpoint name, such as "Headphones (Stealth 700 Gen 2 MAX)" or "ROCCAT Syn Pro Air". The more specific family always wins over the vendor-generic entry, and unrelated products are excluded ("Razer Blade Stealth", "Stealthy Mic", "Reconnect Audio" do not match). An unrecognised device simply gets the generic path, which behaves exactly like a recognised one minus the tailored messages.

---

## 3. What Flubsound does automatically

Implemented in `core/include/flub/engine/DeviceProfiles.h` and `presets/devices/device-profiles.json`, and applied by the app's `EngineController` whenever the output device starts or changes. The JSON is compiled in as `core/src/engine/DeviceProfilesData.cpp` (regenerate with `python3 tools/scripts/embed-device-profiles.py`; `tests/test_device_profiles.cpp` fails if the two drift apart). A file at `<user application data>/Flubsound/device-profiles.json` replaces the built-in database, which lets the lab test new entries without a rebuild.

1. **Identify** the output device by name and family, and the connection type from the platform layer where it can tell (`AudioEndpoints::queryOutputTransport`: Windows endpoint enumerator name and form factor, macOS `kAudioDevicePropertyTransportType`; on Linux it returns *unknown*), falling back to name and format heuristics. Examples: "Hands-Free AG Audio", or ≤ 16 kHz mono ⇒ Bluetooth hands-free; a Bluetooth endpoint at ≤ 16 kHz is treated as hands-free too. When neither tells, the matched family's typical connection is assumed (Stealth, Atlas, ROCCAT, Xbox: USB; Recon, Elite Pro: analog), which is why the screenshot below reads "USB / wireless dongle".
2. **Cap the true-peak ceiling:** −1 dBTP (wired, USB, 2.4 GHz), −2 dBTP (Bluetooth A2DP), −3 dBTP (Bluetooth hands-free). The cap is applied to the app's master limiter, so user presets are not modified.
3. **Adapt to the sample rate:** every module derives its coefficients from the device rate, and alias-prone processing (the air exciter) is disabled below 42 kHz.
4. **Advise:** show the profile's guidance in the app, most important first. A banner under the header (`app/Source/ui/DeviceAdviceBanner.*`) appears when there is something device-specific to say (a recognised profile, or a Bluetooth / hands-free connection). It names the recognised family, the connection and the ceiling in force, shows the top piece of advice (the tooltip lists all of it), and offers the suggested preset with one click (*Use Competitive FPS*) when that preset is not already loaded. *Details* opens Settings, whose Audio page summarises the device, ceiling and suggested preset with the first two messages. The banner can be dismissed per output device for the session. The messages cover:
   - turn off Superhuman Hearing, EQ presets or virtual surround when they would stack with Flubsound,
   - keep voice chat on the headset's own Chat output when it has one,
   - Bluetooth latency and hands-free quality notes, and a low-sample-rate note.
5. **Suggest a preset** for the current mode: *Competitive FPS* (Gaming) / *Flubsound Signature* (Music) for the Turtle Beach and Xbox profiles, or *Bluetooth Headphones* on any Bluetooth connection.
6. **Survive power cycles and replugging.** A 2.4 GHz transmitter keeps the device present when the headset sleeps. When a USB headset is unplugged, JUCE falls back to another output; the app remembers the *preferred* output and switches back as soon as it is listed again (immediately when the OS reports the change, otherwise at the next 5 s rescan).

![Headset advice banner for a Turtle Beach Stealth headset in Gaming mode](images/app-gaming-headset-advice.png)

*Rendered headlessly with `--screenshot out.png --mode gaming --size 1440x900 --device "Headphones (Stealth 700 Gen 2 MAX)"` (as in CI). The `--device` option simulates the output device (only when no real device is open), so profile matching can be checked without the hardware.*

---

## 4. Recommended setup (all Turtle Beach models)

1. **Connect for quality and latency:** 2.4 GHz USB transmitter or cable first, Bluetooth only for casual listening.
2. **Neutral headset sound:** set the headset or its companion app to its flat / default preset. Turn off **Superhuman Hearing** if you use Flubsound's *Footsteps* macro, and turn off the headset's (or Windows') **virtual surround** if you use Flubsound's *Headphone Virtualizer*. Never run two footstep enhancers or two HRTF stages in series.
3. **Routing** (with the Flubsound virtual endpoints; on Windows and macOS these need the virtual driver / HAL plug-in, which are designed but not built yet, so today a virtual cable or per-app capture feeds the strips instead):
   - Set Windows' default output to *Flubsound System*.
   - Assign the game to *Flubsound Game* (the game can then output 7.1 for the virtualiser).
   - Set Flubsound's physical output to the headset's **Game** endpoint.
   - If the headset has a separate **Chat** endpoint, leave Discord/Teams on it so the headset's game/chat balance still works.
4. **Presets:** *Competitive FPS* (Low Latency profile) for shooters, *Cinematic Adventure* / *7.1 Headphone Surround* for story games, *Flubsound Signature* for music.
5. **Mic:** the headset's microphone and mic monitoring are handled by the headset. Flubsound does not process your microphone in this version.

---

## 5. Validation plan (device lab)

For each family, and for each connection type the family supports, a lab entry becomes `labVerified: true` only after all of these pass:

| # | Test | Pass criterion |
|---|---|---|
| 1 | Playback at the device's native format(s) (typically 48 kHz; also 44.1 / 96 kHz where offered) | No dropouts over a 2 h soak, glitch detector clean |
| 2 | Endpoint name matching | Correct profile id; no false match on other lab devices |
| 3 | Connection detection | Correct `Connection` (USB vs Bluetooth vs hands-free) from the platform layer |
| 4 | Ceiling cap applied to the master limiter | Measured output true peak ≤ cap on a loopback recording |
| 5 | Headset power off/on, transmitter unplug/replug, PC sleep/resume | Audio resumes on the headset within 2 s where the OS reports the device change (within the 5 s rescan period otherwise), no crash, no stuck silence |
| 6 | Bluetooth A2DP ↔ hands-free switch (open the mic in a call app) | Engine re-prepares at 16/8 kHz and back without artefacts; the advice banner updates |
| 7 | Game/Chat dual endpoints (where present) | Game via Flubsound, Chat direct, and the balance control works |
| 8 | Latency measurement (loopback impulse) | Added latency ≤ 12 ms (Balanced) / ≤ 10 ms (Low Latency) on USB / 2.4 GHz (the R1.1 target; estimates in `01-architecture.md` §5.2) |
| 9 | Listening check with on-board DSP neutral vs on | Advice text matches the observed stacking behaviour |

The lab list and results live alongside the QA matrix described in `docs/07-roadmap.md`: work item 1.10 (device matrix, including every Turtle Beach family on each of its connection types) and the "Compatibility" row of §7.
