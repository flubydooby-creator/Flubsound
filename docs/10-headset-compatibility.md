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
| **Bluetooth hands-free (HFP/HSP)**, active while the headset microphone is open | Mono output at 8, 16 or 32 kHz (CVSD, mSBC / LC3-WB, LC3-SWB) | Detected from the endpoint name or format. Ceiling capped at **−3 dBTP**, the air exciter is disabled below 42 kHz to prevent aliasing, the Gaming footsteps band at 3.2 kHz and positional focus are off at 32 kHz and below, and the whole chain runs correctly at 8–32 kHz (tested) | Audio quality is limited by the Bluetooth profile itself. Use the stereo endpoint for game/music audio and a separate mic path if possible. |
| **Console only** (headset plugged into an Xbox / PlayStation / Switch with no PC in the path) | — | **Not applicable.** Flubsound runs on computers and cannot process audio that never passes through one. | Routing console audio through a PC (e.g. a capture card with monitoring) makes it processable. |

### Sample rates and formats

The engine follows whatever rate the device's audio format uses. The chain is tested at **8, 16, 22.05, 24, 32, 44.1, 48, 88.2, 96, 176.4 and 192 kHz**, in Music and Gaming mode, in the default Balanced latency profile, with Boost Intensity and every macro at 100 % and the compressor and saturator on, on a hot stereo programme (a 110 Hz burst plus noise, the right channel at 0.9 × the left):
- output is finite on both channels and not silent,
- the sample peak of **both** output channels stays at or below −1 dBFS, the default −1 dBTP ceiling (the test checks sample peak; the true-peak bound is covered by the limiter and chain tests at 44.1–192 kHz, and for the master limiter at the Bluetooth caps also at 8 and 16 kHz, see §3),
- the chain's algorithmic latency (Balanced) stays below 6 ms at ≥ 22.05 kHz and below 12 ms at the Bluetooth hands-free rates (the FIR oversampling delays are fixed in samples: 92 samples = 11.5 ms at 8 kHz).

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
| **PDP** headsets (Turtle Beach brand) (`turtle-beach-pdp`), e.g. Airlite, LVL50 / LVL40 / LVL30, Victrix | ✓ (mostly wired console headsets: jack or controller output) | ✓ (wireless Airlite for Xbox: via the Xbox Wireless adapter) | — | Usually none | The jack or controller output is the output device; wireless Xbox models connect like other Xbox Wireless headsets |
| **Xbox Wireless** models on PC (`xbox-wireless-headset`) | — | ✓ (Xbox Wireless adapter) | — | Windows Sonic / Dolby Atmos for Headphones | Turn off OS spatial sound with Flubsound's virtualiser |
| Headsets on a **Turtle Beach controller** (`turtle-beach-controller`), e.g. Recon Controller, Recon Cloud, Stealth Ultra | ✓ (the controller's jack) | ✓ (the controller's USB cable or wireless link) | — | Superhuman Hearing and EQ presets on the controller | Keep the controller's mode flat while Gaming mode is active, or answer "Headset enhancement is ON" (§3) |
| Any headset in an **Xbox controller**'s jack (`xbox-controller-headset`), e.g. "Headset Earphone (Xbox Controller)" | ✓ (the controller's jack) | ✓ (USB cable or Xbox Wireless adapter) | — | None in the controller | Keep the headset's own modes neutral |
| Legacy **Ear Force** and any other Turtle Beach device (`turtle-beach-generic`) | ✓ | ✓ | ✓ | Any on-board enhancement | Generic Turtle Beach advice |

Families are matched on whole words in the endpoint name, such as "Headphones (Stealth 700 Gen 2 MAX)", "ROCCAT Syn Pro Air" or "Headphones (PDP Airlite Pro Wireless)". The more specific family always wins over the vendor-generic entry, and unrelated products are excluded ("Razer Blade Stealth", "Stealthy Mic", "Reconnect Audio", "Airliner Lounge Speakers", "Headphones (Victrixx)" do not match). Ordinary words that are also family names ("stealth", "recon", "atlas", "pdp", "elite pro") count only next to "Turtle Beach" (or the family's own vendor word) or a headset word such as "headset", "headphones" or "earphone", never next to a speaker word ("speaker", "ceiling", "soundbar", "TV", "HDMI", ...) or another vendor's name: "Atlas Sound Ceiling Speaker" and "Headphones (Jabra Elite Pro)" match nothing (docs/11 E16; a corpus of 130 endpoint names in `tests/data/endpoint-names-corpus.txt`). An unrecognised device simply gets the generic path, which behaves exactly like a recognised one minus the tailored messages.

---

## 3. What Flubsound does automatically

Implemented in `core/include/flub/engine/DeviceProfiles.h` and `presets/devices/device-profiles.json`, and applied by the app's `EngineController` whenever the output device starts or changes. The JSON is compiled in as `core/src/engine/DeviceProfilesData.cpp` (regenerate with `python3 tools/scripts/embed-device-profiles.py`, check with its `--check` option; `tests/test_device_profiles.cpp` fails if the two drift apart). A file at `<user application data>/Flubsound/device-profiles.json` replaces the built-in database, which lets the lab test new entries without a rebuild.

1. **Identify** the output device by name and family, and the connection type from the platform layer where it can tell (`AudioEndpoints::queryOutputTransport`: Windows endpoint enumerator name and form factor, macOS `kAudioDevicePropertyTransportType`; on Linux it returns *unknown*), falling back to name and format heuristics. Examples: "Hands-Free AG Audio", a BlueZ "head unit" / HFP / HSP node, or ≤ 32 kHz mono ⇒ Bluetooth hands-free; a Bluetooth endpoint at ≤ 32 kHz is treated as hands-free too; "Bluetooth", "BT", a BlueZ node (`bluez_output…`) or "A2DP" ⇒ Bluetooth; "USB", "dongle" or "transmitter" ⇒ USB ("wireless" alone is not evidence: a dongle and Bluetooth are both wireless). Any of this evidence beats the family. When none tells, the matched family's typical connection is assumed (Stealth, Atlas, ROCCAT, Xbox: USB; Recon, Elite Pro, PDP: analog), which is why the screenshot below reads "USB / wireless dongle".
2. **Cap the true-peak ceiling:** −1 dBTP (wired, USB, 2.4 GHz), −2 dBTP (Bluetooth A2DP), −3 dBTP (Bluetooth hands-free). The cap is applied to the app's master limiter, so user presets are not modified. *Headset: the master limiter at the Bluetooth -2 dBTP and hands-free -3 dBTP caps holds the 4x true peak of hot inter-sample-peak material* (`tests/test_protection_gaps.cpp`) takes the cap from `adviceFor()` for a Bluetooth Stealth endpoint (44.1 / 48 kHz) and a hands-free one (8 / 16 kHz), sets it on a `MixEngine` master and feeds two strips of material band-limited to 0.4 fs whose sum peaks 9.1–10.2 dB over the cap in true peak (an fs/4 sine at 45° plus noise). It asserts true peak ≤ cap + 0.1 dB on both the 4× `TruePeakMeter` and an independent 4× interpolator, sample peak ≤ cap and no safety clamp; every case measured at least 0.045 dB below the cap.
3. **Adapt to the sample rate:** every module derives its coefficients from the device rate, and alias-prone processing (the air exciter) is disabled below 42 kHz. At 32 kHz and below (Bluetooth hands-free rates) the Gaming footsteps band at 3.2 kHz and the positional focus are off too (docs/11 E17, E24). The cut-off is a sample-rate switch, not a filter: the chain forces the effective air amount to 0 (from the store and from the Clarity macro alike). *Headset: below 42 kHz (hands-free 8 / 16 kHz, USB 32 kHz) the air exciter is cut off - nothing is added above the input band; at 44.1 / 48 kHz the same setting adds its harmonics and shelf* (`tests/test_protection_gaps.cpp`) checks that with air at 100 % the output at 8, 16 and 32 kHz is the delayed input within 1e−6, while at 44.1 and 48 kHz the 2nd harmonic and the 10 kHz shelf are present.
4. **Advise:** show the profile's guidance in the app, most important first. A banner under the header (`app/Source/ui/DeviceAdviceBanner.*`) appears when there is something device-specific to say (a recognised profile, or a Bluetooth / hands-free connection). It names the recognised family, the connection and the ceiling in force, shows the top piece of advice (the tooltip lists all of it), and offers the suggested preset with one click (*Use Competitive FPS*) when that preset is not already loaded. *Details* opens Settings, whose Audio page summarises the device, ceiling and suggested preset with the first two messages. The banner can be dismissed per output device for the session. The messages cover:
   - turn off Superhuman Hearing, EQ presets or virtual surround when they would stack with Flubsound,
   - keep voice chat on the headset's own Chat output when it has one,
   - Bluetooth latency and hands-free quality notes, and a low-sample-rate note.
5. **Ask about the headset's own enhancement** when the profile says the headset (or its controller or companion app) has on-board DSP ([11 E16](11-enhancement-report.md#e16)): the banner asks "Is the headset's own enhancement on (Superhuman Hearing, on-board EQ or surround)?" until the output has an answer. *Yes, it is ON* caps Gaming Footsteps and Detail at 30 % and turns Flubsound's virtual surround off on every strip for that output, so the two do not stack (the knobs and presets keep their values; CAPPED chips show it on the Boost panel; Settings › Audio switches it). The answer is stored per output endpoint (its Windows endpoint id and hardware id, else the name), so it follows the transmitter to another USB port. Not yet run on the owner's Windows PC.
6. **Suggest a preset** for the current mode: *Competitive FPS* (Gaming) / *Flubsound Signature* (Music) for the Turtle Beach and Xbox profiles, or *Bluetooth Headphones* on any Bluetooth connection.
7. **Survive power cycles and replugging.** A 2.4 GHz transmitter keeps the device present when the headset sleeps. When a USB headset is unplugged, the host picks the output itself ([11 E51](11-enhancement-report.md#e51)): never the virtual cable or the input's loopback partner, but the system default or the first real output, with the *safe speaker profile* (virtualiser off, the bass lift of the bass engine and the preset's EQ capped at +3 dB together, −6 dB) unless that output is headphones. It remembers the chosen output with its Windows endpoint id and the adapter's vendor / product id, so the transmitter is recognised when it comes back on another USB port as "… (2- Stealth 700 Gen 2)" or after a rename (its headphone correction, [11 E15](11-enhancement-report.md#e15), follows it the same way), and switches back as soon as it is listed: at once on Windows (device notifications), otherwise when JUCE reports the change or at the next 5 s rescan. After sleep it re-opens the output if the device stopped calling back. Not yet run on a Windows PC.

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
   - **Windows with per-app capture (today's path without the driver).** A captured app keeps playing to its own output device. If that is the headset Flubsound plays to (a USB dongle or the Xbox Wireless adapter used as the default output), the game would be heard twice, dry and processed a few milliseconds apart, so Flubsound does not capture it: the routing panel marks it amber, *Original also audible*, and lists the other active output devices. Fix: in *Settings › System › Sound › Volume mixer* (Windows 11) or *App volume and device preferences* (Windows 10) set the game's output to a device you do not listen to (unused speakers, a monitor's HDMI output, or a virtual cable such as VB-CABLE); the capture still reaches it there, and only the processed copy plays on the headset (docs/11 E47, [06 §6.10](06-gui.md#610-routingpanel--strips-and-applications)). Or let Flubsound make that move: *Settings › Routing › Move the app's own sound away automatically* (off by default; one click on the amber notice turns it on) moves the app's own output to a silent device (S/PDIF, else an unused HDMI output) before capturing it and sets the app's own device back when it is unassigned, the option goes off or Flubsound quits (docs/11 E47, R4.5). Windows applies a per-app device only to sound an app starts afterwards, so a game that is playing at that moment may stay silent until its playback restarts (the routing panel says *Not heard*). If Flubsound crashes or is killed while an app is moved to a Flubsound endpoint (builds with endpoint routing), the next start moves it back (the route journal, [06 §8](06-gui.md#8-per-app-routing-ux)).
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
| 8 | Latency measurement (loopback impulse) | Added end-to-end latency on USB / 2.4 GHz, over the output path Flubsound uses (virtual driver once built; state the capture path otherwise): **≤ 10 ms in Low Latency** (the R1.1 target); Balanced recorded against its ≈ 12–13 ms estimate, the upper edge of R1.1. Budget and scopes: `01-architecture.md` §5 (process-loopback and PipeWire capture add their own buffering, §5.3). Bluetooth links are measured but excluded, since their codec adds 100–300 ms. Tool: the app's Settings › Audio › *Measure latency…* (device only, through Flubsound, or both; `12-feature-guide.md` › How to measure your latency) or `flubsound-cli latency-probe` |
| 9 | Listening check with on-board DSP neutral vs on | Advice text matches the observed stacking behaviour |

The lab list and results live alongside the QA matrix described in `docs/07-roadmap.md`: work item 1.10 (device matrix, including every Turtle Beach family on each of its connection types) and the "Compatibility" row of §7.
