# 12 — Flubsound Pro — Feature and Listening Guide

This guide is for listening. For every control in the app, the plug-in and the CLI it says what the control does to the sound, with numbers from the measurements in [11](11-enhancement-report.md) (the Status lines of each item) and the tests. It also says what to listen for, which pair of the demo pack lets you hear it, and how to compare it fairly. It ends with a one-page checklist to fill in by ear ([§12](#12-by-ear-checklist)) and the list of what cannot be judged by ear yet ([§13](#13-what-cannot-be-judged-by-ear-yet)).

> **How to read an entry.** *What it does* gives measured numbers: bands in Hz, levels in dB, loudness in LU, times in ms. Each number says where it comes from. **Demo pack** numbers are from `flubsound-cli demo` on the built-in 10 s programmes of the Phase 3 batch 5 tree, loudness-matched as the pack does it. A "band delta" is after minus before per octave band, after the match. **Status** numbers are quoted from the item's Status line in [11](11-enhancement-report.md). Each entry names its demo pairs by their file stem (`music-punch` is `NN-music-punch.1-before.wav` / `.2-after.wav` in the pack; `index.txt` gives each pair's number). The entries of the features that landed in batch 5 - Punch and Impact ([E04](11-enhancement-report.md#e04) steps 3–4, [E20](11-enhancement-report.md#e20)), the Enhanced renderer ([E28](11-enhancement-report.md#e28)), Smart macros ([E34](11-enhancement-report.md#e34)) and the Linux node in the app ([E48](11-enhancement-report.md#e48)) - carry the numbers the batch 5 review re-measured on the final tree.
>
> **Nothing here has been judged by a listening panel.** Every "you should hear" is what the measurement predicts. Your ears decide. The owner's first real-device test (Windows 11, Turtle Beach headset) already corrected one control: Warmth "seemed to do nothing", which was true of its first version, and it was rebuilt ([E14](11-enhancement-report.md#e14)).

---

## 0. How to listen and compare

Louder almost always sounds better: brighter, fuller and "more detailed", even when nothing else changed. A difference of 0.5 dB is enough to prefer the louder side. Every comparison in Flubsound is therefore **loudness matched**: the louder side is turned down to the quieter one, never the quieter one raised (so nothing is pushed into clipping). Judge every feature matched, unless its job is the level itself (the Loudness macro, Dynamic Range, Night, the levelling presets, the listening-level cap, ChatMix and the chat duck). Their entries say so.

### The demo pack (`flubsound-cli demo`)

- **What it is:** before / after WAV pairs for every control in this guide, rendered through the same processing chain the app runs, with an `index.txt` that gives each pair's settings, its loudness, its band deltas (as `flubsound-cli analyze --bands` reads the two files), the chain's readouts and what to listen for. There are 63 pairs on built-in programmes: music (drums, bass line, chord pad, a sung lead), the music mastered loud, speech, speech over a hiss floor, a game scene (ambience, footsteps walking left to right, gunshots, two explosions, a voice line, a quiet score) and the same scene as a 7.1 bed. The chat pairs use a game scene with a teammate's voice on a Chat strip. `--input song.wav` renders the pairs on your own file instead (the virtualiser pairs keep the 7.1 scene for a stereo file, and the chat pairs keep their built-in scene).
- **How to run it:** `flubsound-cli demo --out demo` (10 s programmes, 24-bit WAV; about 30 s on two cores). Add `--input my-song.wav` for your own music, `--seconds 20` for longer programmes, `--format f32` for float files, `--only neural-voice-cleanup,music-warmth` for just those pairs (numbered in the order rendered). Every CI run of the Linux job also uploads it as the `Flubsound-demo-pack` artifact.
- **Pairs that set the app's own settings** (Smart macros, the headset enhancement cap, the safe speaker cap, a headphone correction, the per-ear profile, the hearing guard, the chat duck, ChatMix) are rendered through the app's mix engine: the strip, a Chat strip where the pair has one, the master limiter at −1 dBTP and the hearing guard. Their index lines are marked `+ app:`.
- **Listen for:** play both files of a pair in turn in any player, at the same volume. Use headphones for the crossfeed, virtualiser, per-ear and positional pairs. Read the pair's *Listen for* line first. Then close your eyes and swap without knowing which is which.
- **Demo pairs:** `music-boost-100`, `gaming-boost-100` (start here: the whole product at full Boost, matched)
- **Limits:** the built-in programmes are synthetic. They are designed to have the parts each feature acts on (kicks, sibilants, footsteps, explosions), but they are not real music or real game audio. Use `--input` for your own material.

### Loudness-matched A/B (the header's A / B buttons)

- **What it is:** each strip has two parameter banks, A and B. A flip is one click-free switch, and it is loudness matched by default (`compare.matched`, right-click A or B): the louder bank is turned down to the quieter one by a trim on the strip's gain in the mix, estimated in the background and refined once from the meters 3.5 s after the flip ([E37](11-enhancement-report.md#e37)). A line under the buttons shows the trim (*B −7.2 dB*, *matched*, *matching…*).
- **What it does to the sound:** nothing in the chain. In the preset browser's test, Lo-Fi Chill → Club Loud read 6.9 LU louder unmatched and −0.1 LU matched (trim −7.0 dB).
- **Listen for:** set up A as you like it, *Copy A to B*, change one thing on B, then flip. With matching on, the louder side cannot win by level.
- **Demo pairs:** `music-boost-50`, `music-boost-100`
- **Limits:** the estimate is made on a music programme; on other material it is approximate until the 3.5 s refinement. A preset load releases the trim.

### Bypass and "proc. +x LU"

- **What it is:** *Bypass* in the header bypasses every strip (click-free and latency-aligned). With *Loudness-matched bypass* on (the default, right-click *Bypass*), the processed side is turned down to the level of the bypassed one on every flip after the first, and a line under the button reads how much louder the processed sound was: *proc. +2.9 LU*.
- **Listen for:** the first press plays the bypassed sound at its own level, and the line tells you how much of what you liked was only level. Flip a few times after that: the flips are matched.
- **Demo pairs:** `music-loudness` (what "just louder" sounds like, unmatched)
- **Limits:** the bypass takes the whole chain out, including the per-ear profile ([§7](#7-your-headset-and-your-ears)). The input gain stays in: the bypassed sound is held under the ceiling by its own limiter, so with the input gain far up (+10 dB or more on loud music) the bypassed side is limited hard - it should sound squashed, never crackly (since 2026-10-08 its limiter has no corners where it catches a new peak; before, the soak read clicks there).

### The module ear (hold to hear without)

- **What it is:** the ear icon on each module card of the Advanced view. Hold it to hear the strip without that module; release to hear it with. It is matched like the A/B (the louder side is trimmed), and it is not a parameter change (the preset is not marked modified).
- **Listen for:** hold and release while the part the module acts on plays: the kick for the bass engine, the "s" sounds for Smoothness, the hats for Clarity's air.
- **Demo pairs:** `smoothness`, `bass-boost`, `compressor`
- **Limits:** the ear is enabled only while the module is on (by the preset, Boost or a macro).

### Blind A/B/X

- **What it is:** right-click A or B › *Blind test (A/B/X)…*. Ten trials: X is A or B at random, you answer *X is A* or *X is B* (keys A, B, X to play; 1 / 2 to answer). At the end you get the score and the chance of scoring that well by guessing: *9 of 10 right: p = 0.011. You can reliably tell A from B at the same loudness.*
- **Listen for:** use it for the small differences in this guide (latency profile, crossfeed types, Relative presence, Smart on an open recording). A p above 0.05 means you could not reliably hear a difference.
- **Demo pairs:** `latency-profile`, `crossfeed-meier`
- **Limits:** the test covers the header over the whole window so that the trim line cannot give X away. With matching off it warns that the louder side may give X away.

### Why louder always sounds better

- **What it is:** a warning, not a control. Boost, Loudness and most presets make the sound louder, and a louder sound is judged better. The app and the pack turn the louder side down so that you judge the change itself.
- **Listen for:** in `music-loudness` (not matched) the after file is 6.56 LU louder and sounds "better" at first. Turn your player down by 6.5 dB on the after file and compare again: what is left is denser drums with less jump.
- **Demo pairs:** `music-loudness`

---

## 1. Getting sound through Flubsound

### The four strips

- **What it is:** the desktop app processes four streams side by side, each with its own preset and chain: **Game** (7.1, 8 channels), **Music**, **Chat** and **System** (stereo). The strips are summed and pass a master true-peak limiter (−1 dBTP, or the output device's lower ceiling) before the output. The header's strip selector chooses which strip the window edits. A dot marks a strip that receives audio.
- **What it does to the sound:** each strip adds its chain's latency (per profile, below) plus the master limiter's 1 ms look-ahead (0.5 ms when every strip runs Low Latency). Strips in no sync group keep their own latency, so a Quality Music strip does not delay a Low Latency Game strip.
- **Listen for:** a game on Game and music on Music can have different presets at the same time. The strip's mini meter in the routing panel moves when its app plays.
- **Demo pairs:** `virtualiser` (what the Game strip does with 7.1), `chatmix` (Game and Chat mixed)
- **Limits:** on Windows today the strips are fed by per-process capture or a virtual cable. The Windows virtual-endpoint driver (E46) is designed, not built.

### Assigning apps to strips (per-app capture)

- **What it is:** *Assign app to strip…* in the routing panel (Advanced view). On Windows 10 2004+ / 11 the app is captured per process (the capture follows the process, not the device). On Linux the app is moved to the strip's PipeWire sink. The app chips show the state: playing, idle, not running, or an error with its text.
- **What it does to the sound:** nothing by itself. It decides which strip's processing the app gets.
- **Listen for:** the app should be heard once, processed. Check it by switching the strip's Bypass hotkey (*Ctrl+Alt+Shift+B*; *Ctrl+Alt+B* if you saved that chord before 2026-10-07): the sound should change, not double.
- **Demo pairs:** none (routing has no sound of its own; the strip's meter and the doubling guard below are the checks)
- **Limits:** a captured app that also plays straight to your headset would be heard twice, so Flubsound holds it back (see below; on Windows it can also move the app's own sound away for you). On macOS per-app capture (E49) is not built.

### "Original also audible" (the doubling guard)

- **What it is:** if a captured app still plays straight to the device Flubsound plays to (a headset used for both), you would hear it twice: the original and the processed copy a few milliseconds later, which sounds like comb filtering, phasing or an echo. Flubsound does not start that capture, marks the app's chip with two overlapping amber rings and shows the fix ([E47](11-enhancement-report.md#e47)).
- **The fix (confirmed on the owner's Windows 11 PC):** set that app's output to a device you do not listen to (*Settings › System › Sound › Volume mixer*, or *App volume and device preferences*). The capture keeps capturing it, and only the processed copy reaches your headset. The notice lists spare output devices; with none, a virtual cable works as one.
- **Or let Flubsound do it (Windows):** *Move the app's own sound away automatically* (Settings › Routing; off by default) makes that move for you ([E47](11-enhancement-report.md#e47), R4.5). One click turns it on: the button in the notice's dialog or the chip's menu item, e.g. **Move automatically to Digital Audio (S/PDIF)**. Before Flubsound captures an app that plays to your headset, it sets that app's own output to a silent device, then captures it. Automatic picks an S/PDIF or optical output, else an HDMI / DisplayPort output, and only one that no other (unassigned) app plays to; never your headset, the system default, a device named *Speakers* / *Headphones* / *Headset*, or a virtual cable that feeds Flubsound. You can choose the device yourself in Settings › Routing. The chip's tooltip then says *its own sound moved to …*.
  - **Putting it back.** Flubsound sets the app's own device back when you unassign it, switch the option off or quit Flubsound, and after a crash at the next start. Windows applies that only to sound the app starts from then on: an app that is playing at that moment keeps playing to the silent device, and once Flubsound no longer captures it you hear nothing from it. **Restart its playback** (reload the page, or pause and play) and it plays on its own device again. While it is in that state the routing panel shows an amber *Not heard* notice naming the app (after a quit, the diagnostic log notes it).
  - An app whose output you set yourself (for example Edge on S/PDIF) does not play to the headset, so Flubsound leaves it alone and never "puts it back".
  - If you change an app's output after Flubsound moved it, Flubsound leaves your choice; **Move again** in the notice lets it move the app again.
  - If the silent device is unplugged, an automatic choice moves to the next candidate. A device you chose pauses: the app is held back, and the notice says it is not connected.
- **Listen for:** after the fix, the phasing is gone and Bypass removes the processing completely. With the option on, nothing should be heard from the silent device (nothing is plugged into S/PDIF), and the app should play once, processed. After you unassign the app (or quit Flubsound), pause and play it once: it should then play straight to your headset, unprocessed.
- **Demo pairs:** none (it prevents a doubled sound; it has no sound of its own)
- **Limits (gated):** the move uses an undocumented Windows API (the one behind the Volume mixer). It was checked on the owner's PC (Windows 11 build 26200) with a test process only, not yet with Edge or a game. Some apps choose their output device themselves and keep playing to the headset; Flubsound then keeps them held back and says so. Whether a browser or a game picks up the put-back without a restart of its playback is not checked yet (the test process did not). If you quit Flubsound while a moved app is closed, Windows keeps that app on the silent device until Flubsound sees it again (it is moved back then) or you change it in the Volume mixer.

### Output device, re-plug, sleep and resume

- **What it is:** Settings › Audio chooses the output. Flubsound remembers it by the endpoint's identity, so a wireless dongle moved to another USB port (Windows names it *2- …*) is the same device, and its headphone correction and preferred-output setting follow it ([E51](11-enhancement-report.md#e51)). If the chosen output is missing, Flubsound plays to another one with the **safe speaker profile** (below) and switches back when it returns. *Follow the system default output* (off by default) makes the output follow Windows' default instead.
- **What it does to the sound:** on a fallback to speakers the safe speaker profile applies: virtualiser off, bass lift at most +3 dB, and −6 dB. The banner says *Output fallback: safe speaker profile*.
- **Listen for:** unplug the headset dongle during music: speakers take over quieter and with less bass, not blasting. Plug it back: the headset returns with your settings.
- **Demo pairs:** `safe-speaker-cap`
- **Limits (gated):** the dongle re-plug, hot-plug and sleep / resume have been tested on a fake device only; the owner's Windows 11 run with the Turtle Beach dongle is the real check.

### The safe speaker profile's bass cap

- **What it is:** what a fallback to speakers plays: the bass engine's boost and the EQ's positive low shelves and bells at or below 150 Hz are scaled down together until their sum is at most +3 dB ([E51](11-enhancement-report.md#e51)).
- **What it does to the sound:** Status: a strip voiced with +9 dB bass boost, a +6 dB low shelf and a +4 dB bell at 60 Hz reads +17.3 dB at 40 Hz re 1 kHz on the headset and +2.74 dB on the fallback. Demo pack (bass boost 9 dB at Boost 100, the music 12 dB down): the bass lift as applied +14.0 → +3.0 dB, and after the match the low end sits about 3.8 dB lower against the highs (31.5 Hz −0.8 dB, 8–16 kHz +2.9 / +3.0 dB).
- **Listen for:** much less low end; mids and highs unchanged.
- **Demo pairs:** `safe-speaker-cap`

### The feedback-loop guard

- **What it is:** if the output is the playback side of the cable that feeds a strip (for example *CABLE Input* as output while *CABLE Output* feeds Game), Flubsound's output would loop back into itself and howl. The guard holds the output at silence and shows *Output muted: feedback loop* with **Retry**, **Choose output** and **Sound settings** ([E51](11-enhancement-report.md#e51)). A pair you monitor on purpose can be allowed in Settings › Audio (**Allow this pair**).
- **Demo pairs:** none (it prevents a howl; it has no sound of its own)

### Audio device types (Windows Audio, exclusive mode, ASIO, PipeWire)

- **What it is:** Settings › Audio's *Audio device type*. Windows: *Windows Audio (Low Latency Mode)* (the default), *Windows Audio* and *Windows Audio (Exclusive Mode)*; *ASIO* only in a build made with Steinberg's ASIO SDK (`-DFLUB_ASIO=ON`, not the default; [R1.2](TRACEABILITY.md)). macOS: CoreAudio. Linux: ALSA, JACK and, when PipeWire's client library is installed, Flubsound's own *PipeWire* node (the default on a first start when PipeWire plays the audio; on a PulseAudio desktop the first start keeps ALSA, where PulseAudio's ALSA plug-in carries the sound, even with PipeWire's library installed). When the type has a catch, a **DEVICE TYPE** note under the selector says so: ASIO drivers and exclusive mode usually serve one application, so another app reaches your headset only through Flubsound: assign it to a strip (the per-app capture) and set its own output to a device you do not listen to, as in *"Original also audible"* above (exclusive mode can also take it from a virtual cable on the input), or pick a shared type; the PipeWire node makes and links its sinks itself. The exclusive-mode note also points to *Move the app's own sound away automatically* (Settings › Routing, off by default), which makes that move for you; the ASIO note says that switch cannot do it with ASIO: Flubsound cannot tell which Windows device an ASIO driver plays to, so neither the doubling guard nor the automatic move acts there.
- **What it does to the sound:** nothing; it decides who else can use the output and how much delay the driver adds (exclusive mode and ASIO can run smaller buffers). The header's CPU line shows the device's xruns (`· 3 xr`) for every type that counts them, the PipeWire node included.
- **Listen for:** with ASIO or exclusive mode, another app playing straight to the same output is silent (or Windows refuses it). Assign it to a strip and move its own output to a device you do not listen to (*Settings › System › Sound › Volume mixer*; with exclusive mode, *Move the app's own sound away automatically* can do it instead): it comes back once, processed, and the strip's Bypass (*Ctrl+Alt+Shift+B*) changes the sound without doubling it.
- **Demo pairs:** none (device choice; no sound of its own)
- **Limits (gated):** ASIO has been built only by CI with the GPLv3 SDK (the `asio` job) and never run with a real ASIO driver; a published build with ASIO needs the owner's licence choice (docs/02 §6). The per-app way in is confirmed on the owner's PC with the shared types, not yet with exclusive mode or ASIO. The automatic move acts on an app Windows shows playing to Flubsound's output; whether Windows still shows one there while exclusive mode holds the device is untested.

### Latency profiles: Quality, Balanced, Low Latency

- **What it is:** Settings › Processing › *Latency profile*, for every strip. The profile decides the look-aheads and the oversampling ([01 §5](01-architecture.md#5-latency-budget)).
- **What it does to the sound and the delay (48 kHz):**

  | | Quality | Balanced (default) | Low Latency |
  |---|---|---|---|
  | Chain latency | 29.2 ms | 4.0 ms | 2.1 ms |
  | App engine (with the master limiter) | 30.6 ms | 5.4 ms | 3.0 ms |
  | Added end to end (estimate, driver path) | music and batch use | ≈ 12–13 ms | ≈ 9.5 ms |
  | Noise gate | yes (21.3 ms STFT) | no | no |
  | Saturator oversampling | 8× below 88.2 kHz | 4× | 4× |
  | Compressor / limiter look-ahead | 3 ms / 2 ms | 1 ms / 1.5 ms | 0.5 ms / 0.5 ms |

  Status ([E10](11-enhancement-report.md#e10)): the saturator at 24 dB of Tape drive aliases −73.0 dBc in Quality against −54.1 dBc in Balanced (44.1 kHz). Demo pack (Boost 100 + Warmth 100, Low Latency → Quality): every band within 0.1 dB, limiter GR the same (−3.6 dB).
- **Listen for:** at normal settings the profiles sound the same; Quality is cleaner only on hard-driven saturation (hats and sibilants less gritty). In a game, Low Latency's shorter delay is what matters, not the sound.
- **Fair A/B:** blind A/B/X; a difference this small is only real if you pass it.
- **Demo pairs:** `latency-profile`, `noise-gate` (the gate runs only in Quality)
- **Limits (gated):** the end-to-end figures are estimates until the loopback probe is run on real devices: in the app, Settings › Audio › *Measure latency…* ([below](#how-to-measure-your-latency-settings--audio)), or `flubsound-cli latency-probe` ([E42](11-enhancement-report.md#e42)). With *Automatic buffer size* on (the default) the profile also picks the device buffer where the device offers more than one size: Low Latency the smallest (at least 1.3 ms), Balanced about 5 ms, Quality the default ([E42](11-enhancement-report.md#e42) E42c); the Stealth 600PC Gen 3's dongle offers only 480 samples (10 ms). On Linux the profile also sets PipeWire's quantum (256/48000 for Quality and Balanced, 128/48000 locked for Low Latency).

### How to measure your latency (Settings › Audio)

- **What it is:** Settings › Audio › LATENCY › **Measure latency…** ([E42](11-enhancement-report.md#e42) E42d). Flubsound plays short test sweeps (a rising tone) on the output device and records the input device in the same moment, so the delay between them, the *round trip*, is measured instead of estimated. Choose **Device only** (the device's own round trip), **Through Flubsound** (the sweep plays through the selected strip, so Flubsound's engine is in it) or **Both** (the default: the two one after the other, and Flubsound's own share as their difference). Above the button, **Automatic buffer size** (on by default, [E42c](11-enhancement-report.md#e42)) lets the latency profile pick the device buffer: Low Latency the smallest the device offers (at least 1.3 ms), Balanced about 5 ms, Quality the device's default. Dropouts raise it by themselves (to at least twice the size, at most the device's default), and a size you pick in the list above turns Automatic off; on the first start with this switch, a size you had picked there before keeps it off. The buffer follows the profile you chose; the automatic overload response's step (Settings › Processing) never makes it smaller, also after a restart.
- **What it does to the sound:** nothing to your music. While it measures (about 7 s per pass, 13 s for Both) other sound stops and only the sweeps play: −24 dBFS peak, never above −18 dBFS through Flubsound. *Both* plays its two passes back to back, so your sound does not come back between them; *Through Flubsound* fades the output out at its start (what was playing would otherwise run on through the strip's delay) and opens it for the first sweep. They play once, never in a loop, and **Cancel** fades them out at once (through Flubsound the output stays silent for a few milliseconds while the strip lets go of the sweep, then your sound fades back in at its own level).
- **How to measure with the Stealth 600PC Gen 3's microphone:**
  1. Settings › Audio: output *Speakers (Stealth 600PC Gen 3)*, input *Microphone (Stealth 600PC Gen 3)*. The LATENCY box shows *Buffer 480 samples (10.0 ms) … The device offers only this size in this mode*. Expected: the dongle has no smaller period.
  2. Turn the Windows volume (or the headset's dial) down to a comfortable level. In Turtle Beach Swarm II turn **mic monitoring** (sidetone) off: it adds a second path that makes the result ambiguous. Leave the headset's other settings as you use them (its own processing is part of what you hear).
  3. Take the headset off. With the microphone flipped down (unmuted), turn one earcup so its speaker faces the microphone's tip and hold them about 1 cm apart (touching the cushion is fine), in a quiet room. Laying the headset on a pillow keeps them still.
  4. Pick **Both** and press **Measure latency…**, read the dialog, press **Start** and keep still until the progress line ends (about 13 s).
  5. Read the result (below). Measure 2–3 times: the round trip should repeat within a few tenths of a millisecond. Try once with the Low Latency profile too (Settings › Processing).
  6. Send the lines (or the log: Settings › Diagnostics › Export diagnostics; each result is logged as *Latency measured …*) for the docs/11 E42 Status line.
- **How to measure with a cable** (any interface with a line output and a line input, or a 3.5 mm cable from the headphone jack into a line-in; never into a microphone input with its boost on): connect the output to the input's first or second channel (the measurement records the input's first two channels), choose that output and input in Settings › Audio, set the input level so the sweep does not clip, then steps 4–5. A cable has no air path, so the round trip is the device alone; one device's own output and input share a clock, which keeps the runs together (two separate devices drift, and the result says so).
- **If it says *not found reliably*:** hold the microphone closer, raise the volume a little, turn off any microphone noise reduction in Swarm II (it can treat a steady tone as noise) and measure again. *Recorded silence*: the microphone is muted (flipped up) or another input is selected.
- **Reading the result:** *Round trip*: what came back, in ms and samples, the median of 5 runs, their spread and the SNR. *Split*: what the device reports for its output and input (plus Flubsound's engine on the Through path) and the rest the device does not report: converters, the USB or wireless link and the air or the cable. *Playback latency (what you hear)*: the reported output side plus half of that rest. The two directions are assumed alike; the bounds put all of the rest on one side or the other. *Confidence*: high, medium, low (the runs disagree, a second arrival nearly as strong as the first, glitches) or no result (with why: too weak, silence, cancelled). For Both, *Flubsound's engine, measured* should match what it reports (5.4 ms on Balanced at 48 kHz) on a strip without the virtualiser. Through a strip with the virtualiser on (the Game strip, selected by default) it reads about 0.15 ms (7 samples) more, and the result says why: the head-related filters start a few samples late; that is the strip's sound, not buffering. A note about the safety cap appears only when the strip itself raised the sweep above −18 dBFS (or, rarely, when sound left over from before the measurement reached it).
- **Demo pairs:** none (a measurement; it has no sound of its own)
- **Limits (gated):** not run on real hardware yet: the fake-device tests find a known round trip to 0.003 samples, but the owner's first run on the Stealth 600PC Gen 3 is the real check. A wireless headset's microphone path has its own delay and processing (noise suppression), so with it the playback figure is an estimate, a cable measures the device alone. Apps captured per process add their capture buffer (reported, not measured). On the Stealth's dongle Automatic changes nothing (480 only); it helps devices whose driver offers smaller periods, and only when the input also does.

### Tournament mode

- **What it is:** a mode for games with anti-cheat. While it is on, Flubsound stops watching which app is in front (no automatic profiles), stops opening game processes for per-app routing, and shows no on-screen display ([E55](11-enhancement-report.md#e55)). It switches on by itself while a known anti-cheat service runs (*Tournament mode when an anti-cheat runs*, on by default), with one tray notice, and returns to your choice 20 s after the service stops. Tray menu › *Tournament mode*; a badge in the header.
- **What it does to the sound:** nothing. Presets, Boost and the macros keep working; only automation and process access stop.
- **Demo pairs:** none (no sound of its own)
- **Limits (gated):** the Windows service query and the 1 h ETW trace with anti-cheat titles need the owner's PC.

### Linux: PipeWire sinks and the native node

- **What it is:** on Linux the strips are PipeWire sinks (`platform/linux/flubsound-pipewire-setup.sh install` creates them); an app is assigned by moving it to its strip's sink, and Flubsound links each sink's monitor to its input by itself. The app also offers a native **PipeWire** device ([E48](11-enhancement-report.md#e48)): on a first start with a PipeWire server it opens it by itself, creates the strips' sinks, maps every strip (Game, Music, Chat, System) and links them with no setup script, and the routing panel shows a link line (*PipeWire: Linked 14 of 14 input channels …*, amber when something is missing). PipeWire's library is opened when Flubsound starts, not required to start it: without it the PipeWire entry is simply missing and ALSA / JACK work as before, and on a PulseAudio desktop a first start keeps ALSA even with the library installed (the node is preferred only when PipeWire has an output). The node counts its own xruns (the header's `xr`), and if PipeWire removes it, Flubsound shows the device error and re-opens it by itself within about a second and a half (a PipeWire restart takes the same path but has not been tested). Tested against a headless PipeWire 1.0 + WirePlumber 0.4, not yet on a desktop distribution.
- **Demo pairs:** none (routing; the sound is the strip's chain)

---

## 2. Modes and presets

### Music mode and Gaming mode

- **What it is:** the header's mode switch, per strip and per bank. It chooses the macro set, the dynamic EQ's internal mode bands (Music: de-harsh, air, de-boom; Gaming: footsteps, anti-masking, voice) and what Boost does. A fresh Game strip (or any strip with more than 2 channels) starts in Gaming.
- **What it does to the sound:** Boost 0 → 100 in Music leans on clarity, width, bass and loudness; in Gaming on detail, direction, impact and loudness (see Boost). Demo pack, matched: Music Boost 100 rendered 5.65 dB louder and +1.3 … +1.9 dB above 250 Hz after the match; Gaming Boost 100 6.74 dB louder, +0.4 dB at 2–4 kHz and −1.0 dB at 16 kHz.
- **Listen for:** switch the mode on the same music: Gaming's macros aim at cues, not at musical tone.
- **Demo pairs:** `music-boost-100`, `gaming-boost-100`

### The preset browser and its matched preview

- **What it is:** a click on the preset name opens the browser: search by name, tag or description, filter by mode, category, tag, favourites, recent, and *For <your headset>*. The selected row plays on the strip (**Preview**, on by default), matched in loudness to your current sound (**Match loudness**, on by default). *Cancel* puts your sound back; *Load* loads it ([E40](11-enhancement-report.md#e40), [E37](11-enhancement-report.md#e37)). The preview is a never-saved audition bank: a *Save* or *Copy A → B* during a preview takes the sound from before it.
- **What it does to the sound:** the preview plays the preset; matching turns the louder side down by up to 20 dB, estimated in the background at the level your programme arrives at.
- **Listen for:** move between *Current sound* and a preset row: the two play at the same loudness, so what you hear is the voicing.
- **Demo pairs:** `preset-rock-metal`, `preset-orchestral-film`
- **Limits:** there is no hover audition (the selection plays). The estimate is made on music.

### A / B banks

- **What it is:** two banks per strip; see [§0](#loudness-matched-ab-the-headers-a--b-buttons). *Copy A to B* and *Copy B to A* on the copy button. Mode is stored per bank, so A can be Music and B Gaming.
- **Demo pairs:** `music-boost-50`

### User presets, rename and recovery

- **What it is:** the preset menu (…): *Save*, *Save as…* (name, category, description), *Rename…* (keeps the preset's identity, so automatic profiles keep it), *Delete* (to the trash), *Import…*, *Export…*, *Show preset folder*, *Reset strip to defaults*. A preset never changes the latency profile or Bypass. A damaged settings file is restored from its backup with a notice.
- **What it does to the sound:** a saved preset is the sound of its bank. A preset with an unknown or out-of-range key loads with a notice (*unknown parameter "bost" ignored (did you mean "boost"?)*).
- **Demo pairs:** none (file handling; the sound is the preset's)

### The factory presets

- **What it is:** 31 presets: 19 Music (including Podcast & Voice, Late Night Low Volume and Voice Chat), 9 Gaming and 3 Device. Every one states its intent (a tone envelope per octave band, a loudness offset, an LRA bound, a THD+N limit and, in Gaming, a step / bed contrast), and a test checks each against renders ([E14](11-enhancement-report.md#e14)). The numbers below are from the intent blocks (tone on pink re the loudness change) unless a demo pair is named.

  | Preset | In one line |
  |---|---|
  | Flubsound Signature | the everyday balanced sound: a touch of sub-bass, tighter low end, clearer mids, a little air |
  | Punchy Pop | kick and snare transients, tightened and centred bass below 90 Hz, less low-mid boxiness |
  | Bass Head | deep controlled low end: +4.3 dB at 31.5 Hz, harmonic bass for small drivers |
  | Club Loud | big, dense and loud: +6.2 / +7.4 LU, capped by a −9 LUFS loudness target |
  | Crystal Clarity | detail without harshness: mud trimmed, presence rises only when 3 kHz is quiet |
  | Audiophile Subtle | almost transparent, a trace of sub-bass and air (made for Quality) |
  | Wide Stage | sides widened above 220 Hz, a little space |
  | Warm Vinyl | oversampled tape saturation (Tape grit), a soft top |
  | Lo-Fi Chill | tape saturation, top rolled off above 10 kHz (−6.5 dB at 16 kHz), softened transients |
  | Classical & Jazz Dynamic | no drive, no clipper, no compression: only a slow limiter |
  | Rock & Metal | 250 / 500 Hz −1.0 / −0.7 dB, 1 / 2 kHz +0.5 / +1.0 dB, 4 / 8 kHz −0.4 / −0.5 dB |
  | Orchestral & Film | 31.5 / 63 Hz +1.7 / +1.4 dB, 2 / 4 kHz −0.8 dB, Meier crossfeed, LRA loss 0.15 LU |
  | Acoustic & Singer-Songwriter | Warmth 35 %: 125 / 250 Hz +0.8 dB, 8 kHz −0.5 dB, a light tube colour |
  | R&B & Vocal | 31.5 / 63 Hz +1.4 / +1.1 dB, 2 kHz +0.6 dB, 8 / 16 kHz −0.8 / −0.6 dB |
  | Electronic & Ambient | 31.5 Hz +2.3 dB, the rest within 0.3 dB, width 1.15, space 0.2 |
  | Synthwave | synthwave, synth-pop, progressive house / electro: 31.5 / 63 Hz +2.2 / +1.3 dB with 125 Hz (the kick) at 0.0, Warmth 30 % (Tube), 2 / 4 kHz +0.5 / +0.7 dB over 500 Hz–1 kHz at −0.8 / −0.6 dB (the leads forward), width 1.1 above 220 Hz, space 0.15; +1.3 LU on pink, LRA loss 0.3 LU |
  | Podcast & Voice, Late Night Low Volume, Voice Chat | levellers: see [§5](#5-night-and-voice) |
  | Competitive FPS | footsteps lifted as they rise out of the ambience: step / bed contrast +5.4 dB |
  | Battle Royale | the same for large maps: contrast +5.0 dB |
  | Horror Detail | upward compression, Detail and Footsteps for the quietest sounds |
  | Cinematic Adventure | weight and impact on hits and explosions, the score and dialogue open |
  | MOBA & Strategy | voice lines and announcer clear through team fights |
  | Racing | engine note body without drone, tyre and grip cues |
  | 7.1 Headphone Surround | the virtualiser on all eight channels for a 7.1 game |
  | Night Mode Gaming | Auto Level, the Startle Guard at 20 LU and a two-way compressor: [§5](#5-night-and-voice) |
  | Tournament Clean | everything off but the true-peak limiter at −1 dBTP: 0.0 everywhere |
  | Laptop Speakers | small-speaker mode: harmonics instead of lows (−15.5 / −8.9 dB at 31.5 / 63 Hz) |
  | Earbuds | sub-bass weight small drivers lose, dynamic taming of harsh peaks |
  | Bluetooth Headphones | −2 dBTP ceiling for lossy codecs, a gentle clipper |

- **Demo pack (defaults → preset, matched):** Rock & Metal 250 Hz −0.6, 2–16 kHz +1.3 … +2.3 dB; Orchestral & Film 2 / 4 kHz −1.0 / −1.4 dB; Acoustic 250 Hz +0.3, 8 kHz −0.4 dB; R&B 2 / 4 kHz +0.7 / +1.0 dB; Electronic 31.5 / 63 Hz +0.2, 4–16 kHz +0.3 … +0.5 dB; Synthwave 2 / 4 / 8 / 16 kHz +1.1 / +1.9 / +0.7 / +0.4 dB, 125 Hz–1 kHz −0.2 … −0.3 dB (its sub shelf is held back by the headroom protection on this bass-heavy programme: 31.5 / 63 Hz +0.0 / +0.1 dB).
- **Listen for:** each preset against the flat defaults, matched: the table's tone, not more level.
- **Demo pairs:** `preset-rock-metal`, `preset-orchestral-film`, `preset-acoustic-singer-songwriter`, `preset-rnb-vocal`, `preset-electronic-ambient`, `preset-synthwave`, `preset-late-night`, `preset-podcast-voice`, `preset-voice-chat`
- **Limits (owner decisions and the panel):** the five genre presets are conservative (0.6–1.0 dB from their nearest preset on pink) and no listener has heard them; their voicing is the listening panel's call (E14 step 4). Synthwave (owner request 2026-10-06) has not been heard yet either; it is the first factory preset with Relative presence. Classical & Jazz loses 1.42 LU of LRA on a −10.5 LUFS master (a pinned KnownGap).

---

## 3. Boost Intensity and the macros

The macros add to the preset's own values; they never replace them. At 0 a macro does nothing. The **Safety Governor** scales what Boost and the macros add (down to 30 % of it) when the limiter works too hard or the measured distortion is over its budget ([E06](11-enhancement-report.md#e06)); the Boost dial's inner arc shows the share applied.

### Boost Intensity

- **What it is:** the big dial (0–100 %, default 0), one knob for "more". It engages its stages in order.
- **What it does to the sound:** Music: presence +0.35 (0–50 % of the dial), air +0.30 (10–60 %), transient attack +2 dB (10–60 %), width +20 % (0–50 %), bass boost +5 dB (20–80 %), harmonic bass +30 % (35–90 %), maximizer drive +8 dB (30–100 %), glue +30 % (40–100 %), saturation drive +4 dB (60–100 %) and the LF-first limiter (50–75 %). Gaming: presence +0.30, attack +2 dB, positional focus +0.30, bass +3 dB and drive +6 dB. Above 50 % in Music, Boost couples up to +1 dB of attack to the limiter's work so that kicks keep their onset (Status [E05](11-enhancement-report.md#e05): kick onset − body at Boost 80 / 100 −0.89 / +0.21 → +0.23 / +0.30 dB). Demo pack: Music Boost 50 +0.7 … +1.2 dB at 2–16 kHz (1.18 dB louder before the match); Boost 100 5.65 dB louder, limiter GR max −5.8 dB, clipper active 5.2 % of the time.
- **Listen for:** Boost 50: clearer and wider, hardly louder. Boost 100: denser drums, more bass, a finished-master loudness; listen for harshness and pumping.
- **Fair A/B:** A at Boost 0, B at your Boost, matched; or the pairs.
- **Demo pairs:** `music-boost-50`, `music-boost-100`, `gaming-boost-50`, `gaming-boost-100`
- **Limits:** on a loud master the bass engine's headroom protection holds most of the bass rows back (see the Bass Engine card).

### Punch (Music macro 1)

- **What it is:** harder drum hits: the attack of kicks and snares, not a louder mix.
- **What it does to the sound:** the transient shaper's attack +6 dB and its high band (above 4 kHz) +2.5 dB more, through the 3-band shaper of [E04](11-enhancement-report.md#e04) step 3 (Punch no longer drives Tighten, which cut the kick's first 10 ms). Target (E04 Done-when): a kick's 0–10 ms lift at least 2 dB above its 10–30 ms lift at Punch 100; before the remap it was +0.68 dB. Demo pack on the batch 5 tree: 8 / 16 kHz +1.6 / +3.7 dB (the beater's click and the hats' ticks), limiter GR max −0.8 → −2.7 dB; at Boost 100 (before the change below) 16 kHz +6.0 dB and limiter GR −5.8 → −6.5 dB. Measured after the remap (E04 Status): Punch 100 lifts a kick's first 10 ms +5.43 dB against +2.52 dB for its 10–30 ms, onset minus body **+2.91 dB** (target met; +0.68 dB before). At Boost above 60 % Punch's attack eases off (to none from 70 %) so it does not tick in the maximizer (owner decision 2026-10-06, [E04](11-enhancement-report.md#e04)).
- **Listen for:** the first few milliseconds of each kick and snare: a click and a thump at the start, with the level between hits unchanged. If the hits only get louder, or the top end gets splashy, Punch is overdone.
- **Demo pairs:** `music-punch`, `music-punch-boost-100`, `attack-high`
- **Limits:** the remap re-voiced every factory preset that uses Punch, Footsteps, Impact or Detail (21 of 30; integrated loudness of their golden renders −0.20 … +0.05 LU, e.g. Rock & Metal −17.10 → −17.30 LUFS): they sound different from batch 4, most on dense mixes (Punch's bands read fewer of a dense mix's small onsets than the full-band shaper did).

### Width (Music macro 2)

- **What it is:** a wider stereo image and a little more space.
- **What it does to the sound:** stereo width +60 % (to 160 % at 100) above the width low cut (180 Hz) and space +35 % (from 40 % of the knob). The widener is mono-safe: *Mono Safety* (on by default) keeps the image from going out of phase. Demo pack: every band within 0.1 dB after the match (width moves the sides, not the tone).
- **Listen for:** the pad and the hats move outwards; the kick, bass and voice stay in the centre. Switch your player to mono: nothing should disappear.
- **Demo pairs:** `music-width`

### Clarity (Music macro 3)

- **What it is:** clearer vocals and cymbals, less mud.
- **What it does to the sound:** presence +0.8, air +0.7 (from 20 %), de-mud +0.5 (up to 70 %), and the dynamic EQ on (Music's de-harsh, air and de-boom bands follow it). Demo pack: 2 / 4 / 8 / 16 kHz +1.2 / +1.7 / +1.5 / +2.2 dB, 250 Hz −0.6 dB. Presence is dynamic: it lifts more when the 2–5 kHz region is quiet. With *Presence Mode* Absolute (the default) a quiet master gets more lift than a loud one (5.21 dB more at −45 than at −12 dBFS); Relative lifts the same at any level ([E07](11-enhancement-report.md#e07), see the Clarity card).
- **Listen for:** the voice and the hats come forward; the boxy 200–500 Hz region cleans up. Too much sounds thin or bright: check the snare and the vowels.
- **Demo pairs:** `music-clarity`, `relative-presence`
- **Limits:** Music Boost 100 + Clarity 100 at protection Normal reads 2.00 dB of level-dependent presence against a 1.5 dB target (a pinned KnownGap: the de-harsh band's fixed threshold).

### Loudness (Music macro 4)

- **What it is:** denser and louder, like a finished master. A level feature.
- **What it does to the sound:** turns the maximizer on, maximizer drive +10 dB (governed), glue +50 % (from 30 %). Demo pack (not matched): +6.56 LU, limiter GR max −0.8 → −6.7 dB (mean −2.0 dB), clipper active 8.6 % of the time.
- **Listen for:** louder and denser; the drums lose some of their jump. That is the price of loudness.
- **Fair A/B:** turn the after side down by the Level line's figure (6.5 dB) in your player, or use the app's matched A/B: then you hear only the density.
- **Demo pairs:** `music-loudness`

### Warmth (Music macro 5) and the Tube / Tape indication

- **What it is:** a softer, darker tone with a fuller body and a touch of tube colour ([E14](11-enhancement-report.md#e14), rebuilt after the owner's test).
- **What it does to the sound:** a tone tilt ahead of the modules: a body bell at 200 Hz (+3.5 dB) and a high shelf (−3.0 dB at 7 kHz), on pink noise +3.5 dB at 200 Hz and −2.5 dB at 10 kHz at 100 %, loudness compensated, gliding over 200 ms; plus the Tube saturator at +0.9 dB drive (a −6 dBFS 1 kHz sine ≤ 0.5 % THD+N, second harmonic above the third). Demo pack: 250 Hz +1.0 dB, 2 / 4 / 8 / 16 kHz −0.8 / −1.4 / −2.6 / −4.0 dB, 31.5 Hz −1.1 dB, matched within 0.02 dB. A chip beside the knob says what Warmth does to the saturator: *TUBE*, *TAPE* (Tape grit on) or *TONE* (your own saturator keeps its type); a click switches Tape grit.
- **Listen for:** the hats gentler, the voice rounder and closer, the low mids fuller. Nothing should sound muffled.
- **Demo pairs:** `music-warmth`, `tape-grit`
- **Limits:** a user preset saved before the remap that raised Warmth now gets the new Warmth unless it sets Tape grit (release note).

### Footsteps (Gaming macro 1)

- **What it is:** steps and movement lifted when they happen, not the whole background.
- **What it does to the sound:** the cue enhancer, two dynamic EQ bands around 2–5 kHz that lift an onset rising out of the ambience and follow the ambience's own level ([E19](11-enhancement-report.md#e19)); and the transient shaper's high band +2 dB (its own band, so a step keeps its lift under an explosion's tail: at Boost 100 + Impact 100 a step 150 ms into an explosion's tail keeps its lift within 0.49 dB of the step alone, −5.22 dB before). Status: the cue lift stays within ±1 dB across −14 / −24 / −40 LUFS with Auto Level driving; step / bed contrast +5.4 dB in Competitive FPS. Demo pack: 2 / 4 kHz +1.5 / +1.8 dB, the lows −0.7 … −0.8 dB after the match.
- **Listen for:** the footsteps (short thumps with a crunch) stand out when they happen; the ambience between them does not rise.
- **Demo pairs:** `gaming-footsteps`, `onboard-cap`
- **Limits:** capped at 30 % while *Headset enhancement is ON* ([§7](#7-your-headset-and-your-ears)); at Bluetooth hands-free rates the top cue band is off ([E17](11-enhancement-report.md#e17)).

### Positional (Gaming macro 2)

- **What it is:** sharper left / right and front / back placement.
- **What it does to the sound:** positional focus +90 % and width +25 % (from 30 %). The focus leaves each source's level difference between the ears alone: on HRTF-rendered sources at 0–180°, no 1/3-octave band moves by more than 0.09 dB at focus 50 or 100 % ([E24](11-enhancement-report.md#e24)). Demo pack: every band within 0.1 dB.
- **Listen for:** the steps' path from left to right and the gunshots right of centre easier to place.
- **Demo pairs:** `gaming-positional`

### Impact (Gaming macro 3)

- **What it is:** bigger explosions and gunshots: more low end and attack on loud events, not on the rumble between them.
- **What it does to the sound:** the bass engine on with an event-keyed LF punch (a governed low-frequency lift of 80–300 ms plus a harmonic burst on each LF onset, [E20](11-enhancement-report.md#e20)) and the transient shaper's low band +4 dB (from 20 %); no static bass boost. Targets (Done-when): the onset window's LF at least +3 dB, a rumble-only window within ±0.5 dB, the ceiling held, and at Boost 100 + Impact 100 a step under an explosion's tail within 1 dB of the same step alone. Measured (E20 Status): the onset window's LF **+4.51 dB**, the rumble alone **+0.37 dB**, a −1 dBFS explosion leaves at −1.05 dBFS with Impact 100 as with 0 (before, the static shelf: +6.15 / +2.40 dB). Demo pack: 125 / 250 Hz +1.1 / +0.6 dB, 2–16 kHz −1.2 … −1.4 dB after the match; at Boost 100 limiter GR max −4.2 → −5.4 dB.
- **Listen for:** the gunshots and explosions start with a bigger thump; the rumble after them and the ambience stay; the footsteps right after a blast stay audible.
- **Demo pairs:** `gaming-impact`, `gaming-impact-boost-100`, `attack-low`
- **Limits:** the gaming presets that use Impact (Competitive FPS, Battle Royale, Cinematic Adventure, Horror Detail, MOBA, Racing, 7.1 Headphone Surround) lost the static shelf in batch 5: their 31.5 Hz band on pink is 0.1 … 1.1 dB lower (Cinematic Adventure 1.8 → 0.7 dB). The burst is not yet counted by the automatic preamp's model.

### Detail (Gaming macro 4)

- **What it is:** quiet sounds (distant steps, reloads, ambience) brought closer to the loud ones.
- **What it does to the sound:** the compressor on with the upward section up to +8 dB, air +0.4 (from 20 %) and the high band's attack +2 dB. In Gaming the upward floor follows the programme's own background, so the bed is not lifted with the cues ([E19](11-enhancement-report.md#e19) step 2: a −60 dBFS pink bed +9.99 → +0.00 dB, a −40 dBFS tone over it +4.14 dB). Demo pack: 16 kHz +1.1 dB, lows −0.3 dB.
- **Listen for:** quiet sounds come up; the explosions stand less far above the rest.
- **Demo pairs:** `gaming-detail`
- **Limits:** capped at 30 % while *Headset enhancement is ON*.

### Voice & Score (Gaming macro 5)

- **What it is:** clearer dialogue and music over the effects, less boom.
- **What it does to the sound:** presence +0.7, de-mud +0.4 (from 20 %) and the internal voice band (around 2 kHz). Demo pack: 2 / 4 kHz +2.0 dB, 125–500 Hz −0.5 dB. Status ([E59](11-enhancement-report.md#e59)): Gaming Boost 100 + Voice & Score 100 lifts presence 1.46 dB more on quiet than on loud pink with Relative presence (6.00 dB with Absolute).
- **Listen for:** the voice line (55–75 % into the scene) and the quiet music come through the effects.
- **Demo pairs:** `gaming-voice-score`, `chat-duck`
- **Limits:** the chat duck takes this lift back while a teammate talks ([§6](#6-chat)).

### Smart macros

- **What it is:** content-aware scaling of what the macros add ([E34](11-enhancement-report.md#e34)): Settings › Processing › *Smart macros on the <strip> strip* (off by default). An analysis of the strip's input reads how limited the programme already is (PLR), its bass share and its brightness.
- **What it does to the sound:** on a limited master (PLR under 10.5 LU, fully under 7.5 LU) Smart takes back the attack the macros add and down to 25 % of their drive; on bass-heavy programme up to half their bass; on bright programme up to half their air. It moves down in 0.25 s and back in 2 s, and needs 0.5 s of programme first. Status (synthetic limited master, PLR 7.4 LU): Punch 0 → 100 changes the loudness −0.14 LU static and −0.04 LU with Smart; Boost 40 + Loudness 60 −0.47 → −0.04 LU; an open programme (PLR 12.8) within 0.02 LU of static. Demo pack (the music mastered loud, Boost 100, Punch 100, Loudness 60): Smart's attack multiplier 0.00, drive 0.25, bass 0.69; 16 kHz −4.0 dB and 31.5 Hz +1.6 dB after the match (less clipped top, more body).
- **Listen for:** on a loud master: less squashed, less distorted, the drums no flatter than the master itself. On an open recording Smart should change nothing.
- **Demo pairs:** `smart-macros`
- **Presets:** a preset can carry Smart (its `smart` flag): loading it sets the strip's switch, a preset without it switches Smart off, and saving a user preset keeps the switch. The command line follows the preset too; `--smart on|off` overrides it (`process`, `batch`, `quality`), and Flubsound FX keeps it in the project.
- **Limits:** the switch is not in the macro area yet, and Smart plays Loudness 60 on a limited master about 1.1 LU quieter (a product call). The MUSHRA against static macros on 10 real masters is gated on the listening panel.

### First Run – Game

- **What it is:** what a fresh Game strip loads the first time: Competitive FPS with Boost capped at 20 % ([E36](11-enhancement-report.md#e36)).
- **What it does to the sound:** step / bed contrast +1.46 / +4.86 / +5.27 dB on 20 / 40 / 80 ms steps at −24 LUFS, for +0.008 LU of bed.
- **Listen for:** footsteps clearly above the ambience without a louder game.
- **Demo pairs:** `gaming-boost-50`, `gaming-footsteps`

### Dynamic Range (the Startle Guard)

- **What it is:** a control beside the macros (Simple view) and on the Compressor card: *Off*, *20 LU*, *15 LU*, *10 LU (Balanced)*, *6 LU (Shield)* ([E21](11-enhancement-report.md#e21)). It holds a sudden loud event to that much over the level you were hearing just before it. In Gaming its Tame band also takes the explosion's low end down with it. A level feature.
- **What it does to the sound:** Status: at 10 LU an explosion's own loudness −6.15 dB with a step 150 ms into it +0.34 dB against no explosion; at 6 LU −8.99 dB. Demo pack (Off → 10 LU, not matched): −7.15 LU overall, 31.5 / 63 Hz −13.5 / −12.1 dB (the explosions), the steps and ambience unchanged.
- **Listen for:** gunshots and explosions no longer jump out; the steps and the ambience stay.
- **Demo pairs:** `startle-guard`
- **Limits:** Night Mode Gaming's first combat event at −24 LUFS reads 1.75 dB over the steady state (its compressor's 3 ms attack; changing it to 1 ms is an owner decision).

### Smoothness

- **What it is:** a slider beside the macros (Simple view) and a *Smooth* key on the Clarity card (0–100 %, default 0) ([E07](11-enhancement-report.md#e07)). It takes back the sibilance and harshness that Boost and the macros added, never the source's own.
- **What it does to the sound:** a de-esser against a reference: the 5–10 kHz band over the 150 Hz–2 kHz body, cut where the processed sound is brighter than the input (up to 9 dB in Music, 6 dB in Gaming). Status: sibilance over voice at Boost 100 + Clarity 100 +1.18 → +0.25 dB (Smoothness 100), at +8 dB input +2.01 → +0.44 dB. Demo pack (speech at Boost 100 + Clarity 100): 8 kHz −0.9 dB, 4 kHz −0.3 dB. The Clarity card shows *Smoothness cut −x dB (5 - 10 kHz)* while it cuts.
- **Listen for:** the "s" and "sh" sounds softer and less piercing; the vowels keep their clarity.
- **Demo pairs:** `smoothness`

---

## 4. The modules (the Advanced view's cards)

Each card has a power switch, its key controls, an ear (hold to hear without, [§0](#the-module-ear-hold-to-hear-without)) and an expand button that shows every parameter of the module. *AUTO* marks a module that the preset leaves off but Boost or a macro engages. Every control's tooltip says in one sentence what you will hear. The CLI takes every key as `--set key=value` (`flubsound-cli params` lists them with their ranges).

### Input gain, output gain and Auto Level

- **What it is:** `input.gain` (−24 … +24 dB, 0) ahead of everything; `output.gain` (−24 … 0 dB, 0) after the maximizer, so it never boosts and the ceiling holds; *Auto Level* (`autolevel.on`, off; target −30 … −10 LUFS, −18) evens the programme's loudness with a 15 s measure, at most 3–4 dB/s ([E21](11-enhancement-report.md#e21)).
- **What it does to the sound:** gain moves the level; Auto Level lifts quiet programme and lowers loud programme towards its target (Night Mode Gaming: at most +6 dB).
- **Listen for:** with Auto Level, a quiet game scene and a loud one end up near the same loudness after a few seconds, without pumping.
- **Demo pairs:** `night`, `preset-late-night`

### Automatic preamp (and "also on hot programme")

- **What it is:** Settings › Processing › *Automatic preamp on the <strip> strip* (`auto.preamp`, off by default, saved with presets) and *… also on hot programme* (`auto.preampHot`, off) ([E11](11-enhancement-report.md#e11)). It turns the input down by the boost the chain is predicted to add (EQ, bass, presence, air, the dynamic EQ, the compressor's net gain, saturation make-up, the surround fold), less an allowance (`auto.preampAllowance`, 0–12 dB, 1 dB).
- **What it does to the sound:** Status: the predicted maximum boost is within 1 dB of the rendered one for 90 of 90 cases (30 presets × Boost 0 / 50 / 100); *hot programme* on a hot master takes the limiter's work (more than 1 dB) from 6.93 → 0.53 % of the time (Signature) and 7.47 → 0.00 % (Punchy Pop). Demo pack (Boost 100): limiter GR max −5.8 → −4.3 dB, clipper active 5.2 → 3.7 %, after the match 31.5 Hz +0.7 dB and 1–16 kHz −0.7 … −1.2 dB.
- **Listen for:** at the same loudness the drums keep more of their attack and the limiter pumps less.
- **Demo pairs:** `auto-preamp`
- **Limits:** a causal preamp lowers a loud section only once its peaks arrive, so a quiet intro followed by a loud section moves by about 1.3–2.6 dB (Status).

### Noise Gate card

- **What it is:** a spectral noise gate that learns the noise floor and lowers what stays near it. Only in the Quality profile (its 21.3 ms frame). Threshold (`gate.threshold`, 0–20 dB over the floor, 6), Reduction (0–40 dB, 12), Attack (5 ms), Release (80 ms), Floor adapt rate (3 dB/s), Freeze noise profile.
- **What it does to the sound:** Demo pack (speech over a −50 dBFS hiss floor, Quality): the hiss (the uncorrelated side signal) −2.0 dB over the whole file, the voice unchanged (every band within 0.05 dB).
- **Listen for:** less hiss in the pauses between phrases; the voice not watery or chopped.
- **Demo pairs:** `noise-gate`

### Parametric EQ card

- **What it is:** 10 bands at the ISO octave centres (32 Hz … 16 kHz, all 0 dB): Bell, Low / High Shelf, Low / High Cut (12–48 dB/oct), Notch, Band Pass; gain ±24 dB, Q 0.1–18, EQ output −24 … +12 dB. Drag the nodes in the analyser (wheel = Q).
- **What it does to the sound:** exactly the curve you draw. Demo pack (band 8, 4 kHz +6 dB): 2 / 4 / 8 kHz +1.4 / +3.7 / +2.6 dB after the match.
- **Listen for:** a plain tone change at the band.
- **Demo pairs:** `eq-bell`
- **Limits:** a headphone correction from AutoEQ or Equalizer APO belongs in Settings › Correction ([§7](#headphone-correction-settings--correction)), not in a preset's EQ.

### Dynamic EQ card

- **What it is:** 4 user bands that act only when their band is loud (or quiet): modes Cut Above, Boost Below, Boost Above, Cut Below; shapes Bell / Low / High Shelf; threshold, ratio, range, static gain, attack, release. The card cycles the user bands; the mode's own 4 internal bands (footsteps, anti-masking and voice in Gaming; de-harsh, air and de-boom in Music) are driven by the macros and shown as amber markers in the analyser.
- **What it does to the sound:** Demo pack (band 3 at 3.5 kHz, Cut Above −30 dB, range 6 dB): 4 kHz −1.6 dB, 2 kHz −0.6 dB, 8 kHz −0.9 dB, only while the band is loud.
- **Listen for:** the loud vowels and snare hits lose their edge; the quiet passages keep theirs.
- **Demo pairs:** `dynamic-eq`

### Bass Engine card

- **What it is:** Boost (`bass.boost`, 0–15 dB, 0) at Freq (30–200 Hz, 70) with Headroom Protect (−30 … 0 dB, −12); Harmonic Bass (0–100 %) above the Speaker Low Limit (40–250 Hz, 120) with its character; Small Speaker Mode; Tighten (0–100 %); Mono Bass Below (0–250 Hz, off); the subsonic filter (0–40 Hz, 20 Hz; 12 or 24 dB/oct) and Split-Band Protection (off).
- **What it does to the sound:** the boost is an adaptive low shelf that withdraws itself when the low end would exceed the headroom limit. Demo pack (the music 12 dB down, 0 → +6 dB): 31.5 / 63 Hz +2.0 / +1.3 dB against the rest after the match. Harmonic bass 60 %: 250 / 500 Hz +1.9 / +1.2 dB and 31.5 / 63 Hz −0.9 / −1.2 dB (overtones instead of the fundamental), limiter GR max −3.5 dB. Tighten 50 %: the kick's tail shorter while its first 10 ms stay (Status [E04](11-enhancement-report.md#e04): −0.20 dB against Tighten 0), after the match highs +1.1 dB (less low-end energy).
- **Listen for:** boost: more thump and bass-line weight; harmonics: the bass deeper on small speakers and earbuds, a little growl on headphones; Tighten: a drier, tighter kick.
- **Demo pairs:** `bass-boost`, `bass-harmonics`, `bass-tighten`
- **Limits:** on a loud master (peaks near 0 dBFS) the default −12 dB headroom protection holds almost all of the boost back: the same +6 dB on the built-in music at its mastered level reads +0.2 dB at 31.5 Hz. Lower Headroom Protect (towards 0 dB) or the input level to get it; Boost's bass rows are held back the same way.

### Clarity card

- **What it is:** Presence (0–100 %) at Presence Frequency (1–6 kHz, 3.2 kHz) with *Presence Mode* Absolute / Relative; Air (0–100 %); De-Mud (0–100 %); Transient Attack and Sustain (±12 dB, all bands) with *Attack Low* and *Attack High* (±12 dB offsets for the band below the low split, 120 Hz, and above 4 kHz); *Smooth* (Smoothness, [§3](#smoothness)).
- **What it does to the sound:** presence is a dynamic bell, air a high-frequency exciter, de-mud a dynamic cut around 200–500 Hz. *Relative* presence reads the presence band against the programme's own 200 Hz–1 kHz body, so the lift no longer grows when the master is quiet (Status [E07](11-enhancement-report.md#e07): 5.21 → 0.00 dB between −45 and −12 dBFS; Absolute is the default). Attack Low / High run the 3-band shaper (Status [E04](11-enhancement-report.md#e04) step 3: +12 dB attack gives +10.1 / +11.0 / +10.4 dB in the low / mid / high band's first 10 ms, within 1 dB of its peak 0.6–0.75 ms after an onset; with both at 0 the shaper is the full-band one, bit for bit). Demo pack: Relative presence on a −20 dB master with Clarity 100: 2 / 4 kHz −0.6 / −0.9 dB against Absolute; Attack Low +6 dB: the kick's onset (limiter GR −0.8 → −2.5 dB), the highs unchanged in character; Attack High +6 dB: 8 / 16 kHz +1.4 / +2.9 dB, the lows unchanged.
- **Listen for:** Relative: a quiet master less bright and forward at 2–5 kHz than with Absolute; Attack Low: a firmer kick start only; Attack High: sharper hats and snare crack only.
- **Demo pairs:** `music-clarity`, `relative-presence`, `attack-low`, `attack-high`, `smoothness`
- **Limits:** any non-zero Attack Low / High brings the bands' timing and crossover phase to all bands; making Relative the default re-voices every preset that lifts presence (owner decision).

### Saturation card

- **What it is:** Type (Tape, Tube, Digital), Drive (0–24 dB), Mix, Output, and *Tape Grit* (`warmth.tapeGrit`: the classic Warmth; it acts with the saturator off too). The note *Tube chosen by Warmth* shows when the Warmth macro picked Tube.
- **What it does to the sound:** added harmonics; Tube is even-order (rounder), Tape softer on peaks, Digital harder. Oversampled with anti-derivative anti-aliasing ([E10](11-enhancement-report.md#e10): Tube at 24 dB aliases −87.6 dBc in Quality at 44.1 kHz). Demo pack (Tube, 12 dB): 500 Hz / 1 kHz +0.9 / +1.1 dB after the match; Tape grit against Warmth's tone at Warmth 100: 16 kHz +3.7 dB, 31.5 Hz +1.2 dB, 250 Hz −1.0 dB.
- **Listen for:** a denser, rounder, slightly gritty voice and bass; too much sounds fuzzy. Tape grit: more distortion and bass, less of the soft top.
- **Demo pairs:** `saturation`, `tape-grit`

### Stereo & Space card

- **What it is:** Width (0–200 %, 100) above Width Low Cut (60–500 Hz, 180), Positional Focus (0–100 %), Space (0–100 %), Headphone Crossfeed (0–100 %, 0) with *Crossfeed Type* Bs2b / Meier / Mono-safe (Bs2b by default), Mono Safety (on) and Min Correlation ([E12](11-enhancement-report.md#e12)).
- **What it does to the sound:** crossfeed sends a filtered, slightly delayed share of each side to the other ear, as speakers do. Demo pack: crossfeed 50 % (Bs2b): 500 Hz–16 kHz −0.6 … −1.5 dB after the match (the sides' highs fold towards the centre); Meier instead of Bs2b: 500 Hz–16 kHz +0.2 … +0.5 dB (keeps more top); Mono-safe: +0.6 … +1.6 dB (the centre untouched, the mono sum unchanged).
- **Listen for (headphones):** crossfeed: the sound less inside your head, the hats and pad slightly inwards, the centre unchanged. Meier: clearer sides. Mono-safe: the centre exactly as before.
- **Demo pairs:** `music-width`, `gaming-positional`, `crossfeed`, `crossfeed-meier`, `crossfeed-mono-safe`
- **Limits:** re-voicing the crossfeed presets onto Meier or Mono-safe waits for the listening panel.

### Headphone Virtualizer card

- **What it is:** renders a 5.1 / 7.1 input (the Game strip) as binaural stereo for headphones: speaker angles (front 30°, side 100°, rear 145°), Head radius (87.5 mm), Room (15 %), LFE level (+10 dB) and fold, Input channels (Auto / Force Surround / Force Stereo), *Game renders own HRTF*, **Renderer** (Classic / Enhanced) and **Front/Back** contrast (0–100 %, 50 %). On a stereo strip it does nothing (the card says so).
- **What it does to the sound:** Status ([E28](11-enhancement-report.md#e28)): virtualiser on against the plain stereo fold within 0.16 LU (level match), with fold headroom that holds a full-scale 7.1 at 0 dBFS before the limiter. Demo pack (7.1 scene, off → on): 125–500 Hz +0.3 … +0.4 dB, 4–16 kHz −1.3 … −2.2 dB. **Enhanced** replaces Classic's −4 dB rear shelf with direction cues: front louder at 4 and 16 kHz, rear at 1 and 10 kHz (front-left against back-left at the near ear +3.34 / +7.11 / +4.25 / +3.48 dB at 1 / 4 / 10 / 16 kHz), a pinna notch at 7.6 kHz in front moving to 7.1 kHz behind, the centre's timbre matched to the sides (tilt −2.78 → −0.07 dB), at 0.65 % of a core (Classic 0.42 %). It also times every speaker to the ear nearer to it (each speaker keeps its own left / right time difference), so a sound on several speakers at once no longer combs that ear: the same click on all seven speakers reads 9.38 dB peak-to-notch in 4–8 kHz (Classic 28.06 dB, Enhanced before 2026-10-08 18.88 dB). Demo pack (re-measured 2026-10-08): Classic → Enhanced 4 / 8 / 16 kHz +0.9 / −1.8 / +4.2 dB (was +0.7 / −2.0 / +3.4); Front/Back 50 → 100 % doubles the cues (4 / 16 kHz +0.9 / +2.4 dB). Classic stays bit-identical (every factory render unchanged).
- **Listen for (headphones):** the footsteps that circle you outside and around your head; with Enhanced, easier to tell in front from behind; check also whether steps passing between two speakers keep their top end instead of turning hollow (measured on impulse responses, not yet heard). Compare Classic and Enhanced with the `enhanced-renderer` pair, or wait a second after switching in the app (below).
- **Demo pairs:** `virtualiser`, `enhanced-renderer`, `virt-front-back`
- **Limits:** Enhanced's comb fix trades by ear: for a sound panned between two speakers on one side, the far ear now combs deeper, around 1.3 kHz, where that ear is only 4–7 dB below the near one and the time difference between the ears still matters most (both ears together comb less for every pair); nobody has heard it yet. Switching the renderer in the app changes the loudness for about half a second while the level match relearns: up to 2.7 dB in the first 0.1 s on content that is the same on every speaker, within 0.35 dB after 0.5 s (it used to take several seconds). Enhanced as the default, and re-voicing the gaming presets for it, wait for the listening panel. A measured-HRTF path (E29) is not built.

### Compressor card

- **What it is:** a look-ahead compressor with a downward section (Threshold −60 … 0 dB, −18; Ratio 1–20, 2.5; Knee 6 dB; Attack 0.1–200 ms, 10; Release 10–2000 ms, 120; Auto Release; Makeup and Auto Makeup; sidechain high-pass 80 Hz; Mix) and an upward one (Upward Threshold −45 dB, Ratio 2, Max Gain 0–18 dB, 0; Floor −75 dB). *Dyn. Range* (the Startle Guard, [§3](#dynamic-range-the-startle-guard)) is on this card and acts with the compressor off too.
- **What it does to the sound:** Demo pack (−24 dB, 4:1, auto make-up): compressor GR max −15.8 dB; after the match 125 Hz–1 kHz +0.2 … +0.4 dB and 2–8 kHz −0.4 … −1.1 dB (the drums' attacks lower).
- **Listen for:** flatter dynamics: the drums jump less and the quiet pad and voice come closer; the drum hits lose some snap.
- **Demo pairs:** `compressor`, `gaming-detail`, `startle-guard`

### Loudness Maximizer card

- **What it is:** Drive (0–24 dB, 0), Ceiling (−12 … 0 dBTP, −1), **Style** (Custom, Transparent, Punchy, Aggressive, Safe), Clipper share and softness, Glue (multiband), Release (auto by default), **LF Limit** (the LF-first limiter), and in the expanded view the clipper's crest gate and depth limit, the loudness target (−24 … −6 LUFS, −14) and the bed-lift budget. A true-peak limiter on a 4× detector holds the ceiling.
- **What it does to the sound:** a named style sets the clipper and the release (the card dims them). Demo pack (drive 9 dB, Custom → style, matched): Transparent limiter GR max −7.0 → −8.8 dB, clipper active 10.6 → 9.2 %; Punchy clipper 20.1 %, limiter −5.9 dB; Aggressive clipper 20.9 %, limiter −4.8 dB; Safe clipper 0 %, limiter −9.8 dB. LF Limit: the low band is limited before the bands are summed, so a kick is taken down in the low band instead of ducking everything (Status [E05](11-enhancement-report.md#e05) at Music Boost 100: kick onset − body −1.18 → +0.21 dB; a 2 kHz probe's dip under kicks 5.37 → 2.55 dB).
- **Listen for:** Transparent: the cleanest loudness; Punchy: harder drum attacks at the same loudness; Aggressive: loud and dense with grit; Safe: the fewest artefacts, slightly softer drums. LF Limit: the mix stops ducking on each kick.
- **Demo pairs:** `music-loudness`, `max-style-transparent`, `max-style-punchy`, `max-style-aggressive`, `max-style-safe`

### Loudness contour (and "Follow the system volume")

- **What it is:** Settings › Processing › Listening level: *Loudness contour on the <strip> strip* (`contour.on`, off by default), the reference (80 phon), the listening level (`contour.level`, 0 … −60 dB), the maximum lift (18 dB), *Follow the system volume* (off by default) with a *Reference volume* and **Use current volume**, and a *Contour now* curve ([E32](11-enhancement-report.md#e32)).
- **What it does to the sound:** at low listening levels the ear loses the bass and the extreme treble first (ISO 226:2023); the contour adds back the difference. Status: 50 Hz tracks ISO 226 within 0.78 dB from 0 to −40 dB; at −30 dB re the reference +12.1 dB at 50 Hz and +4.4 dB at 12.5 kHz with a −9.9 dB level trim, so it does not get louder. Measured through the app at −30 dB: 50 Hz re 1 kHz +11.90 dB (ISO +11.87). Demo pack (listening 30 dB down): after the match 31.5 / 63 Hz +2.5 / +1.7 dB, 1 / 4 kHz −5.1 / −8.0 dB (the mids sit back against the lows).
- **Listen for:** play the pair quietly: with the contour the bass line and kick stay audible at a low volume.
- **Demo pairs:** `contour`
- **Limits (gated):** the system-volume reads on Windows and macOS have been compiled, not run, until the owner's PC and a Mac; the 10-listener comparison against Late Night is gated on the panel.

### Protection strength (Off, Normal, Strict)

- **What it is:** Settings › Processing, or a click on the governor chip, for every strip ([E06](11-enhancement-report.md#e06)). Off (default): the governor scales only what Boost and the macros add, on the limiter's work and the clipper's distortion. Normal: it also measures the audible distortion of everything the chain adds, the output's dynamics (PLR) and the brightness it adds, and it may also scale the preset's own drive, saturation and harmonics. Strict: tighter budgets, down to 0.
- **What it does to the sound:** budgets at Normal: audible residual −35 dB (Music) / −30 dB (Game), limiter −6 dB, presence brightness +3 dB (Music) / +2 dB (Gaming); Strict −41 / −36 dB, −4 dB and 1.5 dB less brightness. Demo pack (Boost 100 and every macro at 100, Off → Normal): limiter GR max −9.4 → −7.1 dB, clipper active 5.2 → 3.2 %, after the match 31.5 / 63 Hz +0.5 / +0.6 dB and 4–16 kHz −1.3 … −1.8 dB.
- **Listen for:** at Normal, less grit and harshness and more jump in the drums, at a slightly less dense sound.
- **Demo pairs:** `protection-normal`
- **Limits:** the budgets' audibility is the listening panel's to confirm; the 3-minute dynamics row is not met on mastered music.

---

## 5. Night and voice

### Night Mode Gaming and the Night hotkey

- **What it is:** the Night Mode Gaming preset, and the *Night listening* hotkey (*Ctrl+Alt+N*), which latches the same dynamics onto the hotkey strip's current preset until pressed again: Auto Level to −14 LUFS (at most +6 dB), Dynamic Range 20 LU, a compressor 3:1 at −18 dB (knee 10 dB, 3 / 250 ms, auto release) and an upward section 2.5:1 below −32 dB up to +6 dB ([E21](11-enhancement-report.md#e21)). A level feature.
- **What it does to the sound:** Status (Competitive FPS with the latch, an ambush scene): the bed before the event +5.00 dB, the event −10.88 dB, the bed after within 0.05 dB. Demo pack (not matched): −1.40 LU, compressor GR max −9.3 dB, Auto Level up to +6.0 dB.
- **Listen for:** the quiet parts come up and the explosions stop jumping out; the whole scene sits in a narrower range for late-night playing.
- **Demo pairs:** `night`
- **Limits:** the first combat event at −24 LUFS reads 1.75 dB over the steady state (owner decision: compressor attack 3 → 1 ms).

### Late Night Low Volume

- **What it is:** a Music preset for listening quietly: Auto Level and a two-way compressor with 12 dB of make-up. A level feature.
- **What it does to the sound:** Status: the output within ±1 LU of −20 LUFS; LRA loss 5.40 LU (it is a leveller by design). Demo pack (defaults → preset, not matched): −4.33 LU, compressor GR max −18.4 dB, Auto Level down to −9.2 dB.
- **Listen for:** at a low volume every part of the song stays audible without the drums jumping out.
- **Demo pairs:** `preset-late-night`

### Podcast & Voice

- **What it is:** a Music preset for podcasts, audiobooks and talk: rumble and boxiness removed, a light spectral gate, Auto Level and compression. A level feature.
- **What it does to the sound:** Status: +8.1 … +8.4 dB over 4–16 kHz on music re its +4.8 LU; LRA loss 5.46 LU. Demo pack (speech, not matched): −0.23 LU, 1 / 2 kHz +1.7 / +1.3 dB, compressor GR max −7.2 dB, Auto Level down to −7.6 dB.
- **Listen for:** quiet and loud phrases at about the same level, the consonants clearer, no rumble.
- **Demo pairs:** `preset-podcast-voice`

### Voice Chat (the Chat strip's preset)

- **What it is:** the preset a fresh Chat strip loads: low cut at 110 Hz, de-mud, presence at 2.8 kHz, a de-esser, Auto Level to −19 LUFS, a compressor 3:1 at −30 dB with +16 dB make-up and an upward section below −42 dB; no maximizer drive or clipper ([E23](11-enhancement-report.md#e23)). A level feature.
- **What it does to the sound:** Status: speech at −35 and −12 LUFS (23 LU apart) ends 2.6 LU apart. Demo pack (speech, not matched): −0.95 LU, 500 Hz / 1 kHz +1.0 / +1.2 dB, 8 kHz −3.3 dB.
- **Listen for:** a quiet teammate and a loud one at about the same level; less rumble and boom.
- **Demo pairs:** `preset-voice-chat`

---

## 6. Chat

### ChatMix

- **What it is:** one balance between the Game and Chat strips ([E22](11-enhancement-report.md#e22)): the tray flyout's *GAME ‹slider› CHAT*, the same slider on the Chat strip's row, and the hotkeys *Ctrl+Alt+PageUp* (more chat) / *PageDown* (more game). The side it moves away from falls; the other stays at 0 dB; both are at 0 dB in the centre. A level feature.
- **What it does to the sound:** gains Game 1 − max (0, b), Chat 1 + min (0, b), gliding over 50 ms. One hotkey press (0.2): Game −1.9 dB; five presses: Game muted. Demo pack (centre → 50 % towards Chat, not matched): the game −6.0 dB (31.5 Hz, where only the game plays), the voice unchanged.
- **Listen for:** the game quieter under the voice; the voice itself does not change.
- **Demo pairs:** `chatmix`
- **Limits:** it needs a Game and a Chat strip; routing a chat app's output to the Chat strip is manual today (Chat-endpoint routing is open).

### Duck game under voice chat

- **What it is:** a switch on the Chat strip's row and in Settings › Processing › *Voice chat*, off by default, with its depth (3–6 dB, 4.5) ([E22](11-enhancement-report.md#e22)). A voice detector on the Chat strip's input drives it.
- **What it does to the sound:** while a teammate talks: a dip in Game and Music around 1–2.4 kHz (Game 1.25 kHz −4.62 dB, Music 2 kHz −4.50 dB at 4.5 dB), the footstep bands kept within 0.5 dB, the Game chain's Voice & Score lift taken back, and the Game strip's peaks held 3 dB under the master ceiling, or lower when a loud teammate needs the room: the game's peaks then make room for the voice under the master ceiling, down to at most 6 dB under it (the room floor, provisional; with the Game fader turned up the room only keeps the voice from adding to what the master limiter already takes off the game); in 30 ms, out over 300 ms. Status: explosions limited to −1 dBFS under speech moved the chat's short-term level 0.20 → 0.00 dB at −20 LUFS, and for a loud teammate at −14 LUFS 1.02 (duck off) → 0.45 dB (0.82 dB before the room, 2026-10-08), the explosions' peaks pulled down by up to 4.7 dB (short-term 1.3 dB) while the teammate talks. Demo pack (not matched; re-rendered 2026-10-08 with the room: byte-identical, because this scene's mix peaks at −3.2 dBTP and never needs the room): duck depth 100 % while the voice talks (held 43 % of the programme), 2 kHz −0.2 dB over the whole mix.
- **Listen for:** while the teammate talks the voice is easier to follow and the steps are still there; before and after, nothing changes. With a loud teammate over loud explosions: the explosions give way a little more while the teammate talks, and the voice does not dip with them; no click when the teammate starts or stops, also with the Game fader turned up.
- **Demo pairs:** `chat-duck`
- **Limits:** the room floor (how far the game's peaks may be pushed down for a loud teammate) is −6 dB for now, the owner's decision: −9 dB holds a −14 LUFS teammate to 0.18 dB at 2.7 dB more pull-down of the explosions' peaks ([E22](11-enhancement-report.md#e22)); no demo pair has a teammate loud enough to need the room; the play test against SteelSeries Sonar with real Discord captures is gated on the owner's PC.

### The voice dot

- **What it is:** a dot on the tray flyout and the Chat row: lit (*Voice*) while the Chat strip carries speech, a ring (*Quiet*) otherwise.
- **What it does to the sound:** nothing; it shows what drives the duck. Status: lit 0.10–0.15 s after speech starts, out about 0.6 s after the last syllable; dark for a 1 kHz tone and for music (0 % false positives on 10 s of drums, pads and noise).
- **Demo pairs:** `chat-duck`

### Neural voice cleanup (experimental)

- **What it is:** a switch in Settings › Processing › *Voice chat*, *Neural voice cleanup on the Chat strip (experimental)*, off by default ([03 §16](03-dsp-design.md#16-neural-voice-cleanup-experimental), [E35](11-enhancement-report.md#e35)). A small neural network (51 k parameters), trained by Flubsound from scratch on synthetic speech and noise, listens to the Chat strip every 5 ms and sets 22 band gains that turn the noise down and leave the voice; Flubsound's first neural model, run by its own runtime (no third-party AI library, nothing leaves the PC). It works at 48 kHz in Balanced and Quality and adds 20 ms to the Chat strip only (with 480-sample buffers; the Game strip is not delayed). A *Status* line under the switch shows whether it runs and, live, the voice activity and how much it cuts; when it cannot run it says why and what to change (the latency profile, the buffer or the sample rate).
- **What it does to the sound:** steady noise (fans, hum, hiss), typing and background voices drop, most of all between words: on the held-out synthetic test set (108 clips of 8 s, six noise types at −5 to +20 dB SNR) −19.9 dB between words (the existing noise gate −6.1 dB, and only in Quality), +4.7 dB SNR overall (gate +3.5 dB), +4.3 dB on typing and +3.4 dB on babble where the gate does almost nothing. The voice changes little: clean speech −0.24 dB, 0.55 dB per band on average (gate 0.33 dB); on nearly clean speech it does more harm than good (SNR 40 → 32 dB). Through the Voice Chat preset the speech-to-pause ratio goes 7.3 → 21.7 dB. Never a hard gate: each band keeps at least −30 dB, and a cut deepens by at most 2.2 dB per 5 ms (it lets go at once). Demo pack (*speech-noisy*: the voice with a fan, mains hum and quieter typing, the noise about 18 dB under the voice, a mild scene where most of the change is between the words): matched; the pause floor (5th percentile of 10 ms frames) goes from about −42 to −70 dBFS.
- **Listen for:** the fan, the hum and the typing lower, most of all between words and phrases; the voice should stay whole. Listen for a thinner or watery voice, chopped word ends, a background that pumps with the voice, and "musical" chirps in the pauses.
- **Demo pairs:** `neural-voice-cleanup`
- **Limits:** experimental. Trained and tested only on synthetic speech and noise: real voices, real microphones and real Discord / Teams codecs are unheard, and the model may cut a soft voice or keep a noise it never saw. 48 kHz only (at 44.1 kHz it stays off and says so); not in Low Latency (one 10 ms frame is less than its 20 ms); with 512-sample or longer buffers it needs more safety frames (1 200 samples = 25 ms), which only Quality allows. It does not dereverberate and has no pitch filter between the harmonics (RNNoise's), so noise right under a vowel stays.

---

## 7. Your headset and your ears

### Device profiles and matching

- **What it is:** Flubsound recognises the output device by its name (and, where the OS gives one, its endpoint identity) and matches it to a profile: every Turtle Beach family, and generic Bluetooth / USB / wired devices ([10](10-headset-compatibility.md), [E16](11-enhancement-report.md#e16)). The banner and Settings › Audio show the profile, the connection, the ceiling the master limiter applies (−1 dBTP; −2 on Bluetooth A2DP; −3 in hands-free), the suggested preset and setup advice.
- **What it does to the sound:** the ceiling, and on Bluetooth hands-free the cue enhancer's top band off; nothing else until you answer the enhancement question below.
- **Listen for:** Settings › Audio should name your headset family (the owner's Windows endpoint names are in the matching corpus: 69 of 69 headset strings in the right family, 0 false positives).
- **Demo pairs:** `onboard-cap` (the part of matching you can hear)
- **Limits (gated):** hardware identity (USB ids) and per-model feature flags wait for the device lab.

### "Headset enhancement is ON" (Superhuman Hearing, on-board EQ)

- **What it is:** a per-headset answer, asked by the banner (*Is the headset's own enhancement on?* **Yes, it is ON** / **No**) and switchable in Settings › Audio ([E16](11-enhancement-report.md#e16)). With it ON, Flubsound caps Gaming Footsteps and Detail at 30 % and holds the virtualiser off, so the headset's own processing and Flubsound's do not stack; *CAPPED* chips show it. It follows the headset through a re-plug.
- **What it does to the sound:** Demo pack (Footsteps 100 + Detail 100, cap off → on, matched): 2 / 4 kHz −0.9 / −1.3 dB, lows +0.6 … +0.9 dB: the cue lift is smaller, expecting the headset to add its own.
- **Listen for:** with Superhuman Hearing on, the steps should be about as clear with the cap as they were without it and without the headset's enhancement, not doubly bright.
- **Demo pairs:** `onboard-cap`
- **Limits (gated):** the on-board processing's own response is unmeasured (device lab); the owner's Turtle Beach run is the real check.

### Bluetooth awareness

- **What it is:** on a Bluetooth connection the ceiling drops to −2 dBTP (lossy codecs overshoot peaks); in hands-free mode (a headset microphone in use, 16 kHz audio) to −3 dBTP, and the cue enhancer's top band is switched off at rates up to 32 kHz (it would lift only noise there) ([E17](11-enhancement-report.md#e17)).
- **What it does to the sound:** Status: at 8 / 16 / 32 kHz sample rates the Footsteps 100 lift at 3.2 kHz went from +7.0 dB over 1 kHz to −0.02 … −0.03 dB.
- **Listen for:** in a hands-free call, no hiss pumping with the footstep band.
- **Demo pairs:** none (it needs a Bluetooth headset; check the ceiling in the banner)
- **Limits:** the runtime hands-free policy and codec detection are open.

### Headphone correction (Settings › Correction)

- **What it is:** a correction curve for the open output device: **Import ParametricEQ.txt…** (AutoEQ or Equalizer APO / Peace files), **Compare** (the filters off, the preamp kept, so the comparison is fair), **Remove**, *Correction on for this output* ([E15](11-enhancement-report.md#e15)). It belongs to the device: presets and A/B never change it, and it follows the device through a re-plug. Flubsound ships no measurement data.
- **What it does to the sound:** the curve on the whole output, with an automatic preamp so it cannot clip; switching devices crossfades in 20 ms. Demo pack (an example curve: +6 dB low shelf at 105 Hz, −4 dB at 3 kHz, matched): 31.5 / 63 Hz +1.9 / +1.5 dB, 2 / 4 kHz −3.8 / −4.9 dB against the level.
- **Listen for:** with your headset's own AutoEQ file, the headset should sound more neutral: less boom, less harshness, depending on its measurement.
- **Demo pairs:** `device-correction`
- **Limits (gated):** not yet run on Windows; two identical headsets on Linux / macOS share a curve.

### The hearing guard: listening-level estimate, dose and cap (Settings › Hearing)

- **What it is:** an **estimate** of the level at your ear, for information: not a measurement, not a hearing test and not a medical device ([E32](11-enhancement-report.md#e32) (c)). It needs your headset's sensitivity (dB SPL of a full-scale tone at full volume), typed in *Use my own sensitivity figure* per output; a profile's manufacturer figure would be prefilled and marked *manufacturer figure, not lab-verified*. With a sensitivity it shows the level now, over 5 s and this session in dB(A) with the system volume it counts, today's and the last 7 days' estimated dose against the WHO / ITU-T H.870 weekly allowance (80 dB(A) for 40 hours), and offers the **listening-level cap** (off; 60–100 dB(A), 85). The loudness panel shows *EST. / DOSE / CAP* while a sensitivity is known.
- **What it does to the sound:** nothing unless the cap is on and the estimate is over it; then a slow gain holds the A-weighted level over any 5 s under the cap. Status: through the app, a 1 kHz tone at −20 dBFS, 100 dB SPL, volume −10 dB read 69.99 dB(A) (hand figure 70.00); a 60 dB(A) cap held it at 59.37 dB(A) within 2 s; a hostile programme at 104.7 dB(A) stayed at most 84.25 dB(A) under an 85 dB(A) cap. Demo pack (110 dB SPL, full volume, cap 75 dB(A), not matched): the loudest 5 s estimated 83.4 dB(A); with the cap −10.2 LU, cap gain down to −13.5 dB, the tone unchanged.
- **Listen for:** with the cap on, loud passages glide down over about a second and stay there; the tone does not change.
- **Demo pairs:** `hearing-cap`
- **Limits (gated):** no shipped profile has a sensitivity (Turtle Beach publishes none that fits), so the guard is off until you enter one; the estimate can be 10–20 dB off (the headset's own volume dial, fit and on-board EQ are unknown to it) until the device lab calibrates it; the Windows and macOS volume reads have not run on real systems; a notification at the weekly allowance and the cap limiting Boost are open.

### Personal profile (per ear)

- **What it is:** Settings › Hearing › *Use my personal profile (per ear)*: a **listening preference**, not a fitting ([E33](11-enhancement-report.md#e33)). **Flat**, *Balance* (±12 dB; it only turns an ear down), and per ear a *Gain* (±12 dB) and eight bands at 250 Hz – 8 kHz (±15 dB), typed or dragged. Each ear's target is at most +15 dB and the ears at most 12 dB apart. Stored in its own file (`personal-profile.json`), applied to every strip.
- **What it does to the sound:** per-ear bells before the compressor, with a headroom reservation that turns both ears down by the louder ear's boost, so the stereo-linked dynamics keep each source's level difference between the ears (Status: within 0.03 dB). Through the app: right +6 dB → R − L +6.00 dB (both ears first −6.0 dB); right +9 dB at 4 kHz → +9.00 dB there and 0.00 dB at 250 Hz. Demo pack (right ear +9 dB at 4 / 6 / 8 kHz, matched): reservation −3.1 dB, the right ear's 8 kHz about 9 dB over the left.
- **Listen for (headphones):** listen with each ear: the hats and the voice's edge brighter on the right; the left ear slightly quieter (the reservation) until the maximizer or the volume gives it back.
- **Demo pairs:** `per-ear`
- **Limits:** the global Bypass takes the profile out too; the bands ripple ±2–3 dB between their frequencies on steep shapes; a +12 dB one-ear high-frequency profile turns both ears down about 9 dB (owner decision on this design).

---

## 8. Readouts and meters

### In / out loudness and "limiter active"

- **What it is:** the Simple view's LOUDNESS card (the strip's short-term loudness, the input as a marker, and *2.9 LU louder than the input* in words); the Advanced view's loudness panel: momentary / short-term / integrated LUFS, LRA, true peak, RMS, gain reduction, correlation, and *LIM* (*Limiter active 12 % of the last 10 s*).
- **Listen for:** a high *LIM* (the limiter working much of the time) with a flat, pumping sound means too much Boost or Loudness for this material.
- **Demo pairs:** `music-loudness`

### The spectrum analyser and its views

- **What it is:** the SPECTRUM + EQ panel: the input spectrum (grey fill), the output (accent line with a glow), peak hold, the +4.5 dB/octave Tilt and the EQ curve with its nodes and the amber dynamic-EQ markers. That view is the default and is unchanged. The **View** chip adds optional views, all off until you switch them on (remembered between sessions, except Freeze); **Diff** and **Freeze** also have chips when the header has room ([06 §6.4.1](06-gui.md#641-optional-views-owner-request-2026-10-06)).
  - *Hover readout:* rest the mouse on the plot (not on a node) for a crosshair and a box with the note and cents (*B1 +7c · 62.0 Hz*, A4 = 440 Hz) and the Out / In levels there (without the tilt). Handy to name a boomy note or a resonance.
  - *What Flubsound changes (Diff):* a green line on the right-hand EQ scale: output minus input per frequency, smoothed. 0 dB = untouched. It includes everything (Boost, macros, dynamics, loudness), not only the EQ you drew, averaged over the music.
  - *Sharper lows:* below 220–300 Hz a longer analysis (about half a second) separates bass notes that the normal view merges (two notes a whole tone apart at 60 Hz: one lump before, two peaks now). Noise and dense mixes read the same; a single held bass note stands about 8 dB taller than in the normal view, because the finer analysis concentrates a tone into a narrower slice.
  - *Spectrogram:* the output as a scrolling picture, newest at the top, about 5.5 s of history; brighter = louder. The EQ and the other overlays stay on top.
  - *Stereo width:* a translucent band at the bottom of the plot: how much of each frequency is side (L − R) rather than centre. Flat at the bottom = mono there (bass usually is), the dashed line = as wide as two unrelated channels, the top = out of phase.
  - *Freeze:* keeps the current output (and input) trace as a dashed reference; press again to re-capture, Shift+click or View › Clear to remove. It stays when you switch strips.
  - *Piano keys:* a small keyboard C1–C8 along the bottom, lined up with the frequency axis; the hovered note's key lights up, and the keys of the notes that are playing glow (a note that stands out over its neighbours; the glow fades over 0.15 s when it stops).
  - *Piano keys: fundamentals only* (with Piano keys on): only the notes actually played light up, not their overtones (a bass note lights one key, not also the octave and the fifth above it); at most one bass note below C3 and six notes in all.
  - *Visualisers* (View › Visualiser; one at a time in place of the spectrum, or beside it on a wide window with *Beside the spectrum*; remembered between sessions; [06 §6.4.2](06-gui.md#642-visualisers-owner-request-2026-10-06)):
    - *Goniometer:* the stereo picture as a glowing trace: a vertical line = mono, leaning up-left / up-right = more left / right, a round cloud = wide, a horizontal line = out of phase. It scales itself (the gain is in the corner).
    - *Stereo field:* one coloured dot per third of an octave (bass at the bottom, treble at the top), placed between L and R where that band sits; a bar instead of a dot = that band is wide, brighter = louder. Width and Crossfeed move the high bars; bass should stay a dot in the middle.
    - *Loudness history:* the last minute of momentary (thin) and short-term (bold) loudness in LUFS, with the loudness target as a dashed line when Auto level or the maximizer's automatic drive sets one. Late Night and Loudness show as a flatter, higher line.
    - *Waveform before / after:* the last 4 seconds of the input (grey) and the output (colour) as peak envelopes: Boost lifts the body, Punch sharpens the attacks, the limiter flattens the tops.
    - *Gain reduction history:* the last 15 seconds of what the compressor, limiter, glue, bass protection and master limiter take off, one coloured line each, with the current values in the legend.
    - *3D waterfall:* the last 6 seconds of the output spectrum as a glowing landscape: bass left, treble right, louder = higher; the live spectrum is the bright front ridge and older ones roll back towards the horizon. A kick is a hill bouncing at the left, a synth lead a ridge line running into the distance.
    - *Radial spectrum:* the output spectrum around a ring, mirrored left and right: bass at the top, treble at the bottom, louder reaches further out; the centre swells on each kick and shows the momentary loudness (LUFS).
    - *Open in a window* (the View menu's last item): any of these views, or the spectrum or spectrogram, in a window of its own that you can put on a second monitor or make full screen (F11 or a double-click; Esc leaves; Left / Right change the view). It remembers its view, place and full screen; it is off in Tournament mode.
    - *Correlation meter* (View › Strip under the plot): a −1 … +1 bar under the spectrum or any visualiser: +1 mono, around 0 wide, red below 0 (partly out of phase: thin on speakers, odd in mono). The small triangle holds the lowest value of the last 3 seconds.
    - *Chord name* (main view or strip): the chord that is playing as a big symbol (*Am7*, *C/E* when E is in the bass, *Fsus2*, *N.C.* when nothing), its notes, a one-octave keyboard lighting the notes that sound, and the last chords (*Am › F › C › G*). It waits about a quarter of a second before showing a new chord, so a passing note does not make it flicker; it spells with flats in flat keys.
    - *Chromagram* (main view or strip): twelve bars C … B, how much of each note the music holds in all octaves together, with the last 15 seconds scrolling beside them and the song's key (with its scale notes marked) on top.
    - *Song key* (strip): the key of what is playing (*F minor · 82 %*, how sure it is), the relative key and the scale's notes; it listens to about the last 15 seconds, so give a new song a few seconds.
- **Check it:** play a sine (an online tone generator) at 55 Hz: the readout names *A1 +0c*. Turn Diff on and raise an EQ band by 6 dB: the green line rises about 6 dB around that band (less if Boost's limiter takes some back). Freeze, change a macro, and compare the live line with the dashed one. Pan a mono song hard to one side in its player (or use a mono / stereo test track): Stereo width jumps from the bottom to about the dashed line, the goniometer's line leans to that side and the stereo field's dots move to that edge. Raise Boost to 100 %: the gain reduction history's limiter line deepens and the waveform's coloured tops flatten. Play a song whose chords you know (or a piano app's chord): Chord name shows them (passing names such as *Am6* can appear between two chords), the chromagram's tallest bars are the chord's notes, and Song key settles on the key within 10–20 s (a song in a minor key may read as its relative major, shown next to it). With *fundamentals only*, a low bass note lights one key instead of a column of overtones.
- **Demo pairs:** none (a display; it has no sound of its own)
- **Limits:** the analyser shows the strip's own output before the strip gain and the master limiter. Sharper lows reacts about half a second later than the normal view in the bass.

### The governor chip, budgets and brightness

- **What it is:** *Safety governor OK*, or amber *Governor NN% · limiter / distortion / dynamics / harmonics / brightness · holding / recovering*; its tooltip gives the readings against their budgets; the loudness panel's PROTECTION section at Normal / Strict shows the audible residual, the output PLR and the presence / harsh / air lifts against theirs ([E06](11-enhancement-report.md#e06), [E38](11-enhancement-report.md#e38)).
- **Listen for:** when the chip turns amber, the governor is taking Boost back; if you hear grit at that moment, raise the protection strength.
- **Demo pairs:** `protection-normal`

### Active-now chips

- **What it is:** chips under the macros for the stages that change the sound right now, from the chain's effective values: *Bass +3.1 dB @ 70 Hz*, *Presence 45 %*, the limiter while it takes more than 0.5 dB, and so on ([E38](11-enhancement-report.md#e38)).
- **Listen for:** a chip that appears when you turn a macro up tells you which stage you are hearing.
- **Demo pairs:** `music-boost-100`

### The Smoothness cut, the Warmth chip and the virtualiser readings

- **What it is:** *Smoothness cut −3.2 dB (5 - 10 kHz)* on the Clarity card while Smoothness cuts; *TUBE / TAPE / TONE* beside Warmth; the virtualiser's level-match make-up and fold headroom on the chain's meters (`render.stats.fold` in the CLI).
- **Demo pairs:** `smoothness`, `music-warmth`, `virtualiser`

### CAPPED chips

- **What it is:** shown while the headset enhancement cap holds Footsteps, Detail or the virtualiser back ([§7](#headset-enhancement-is-on-superhuman-hearing-on-board-eq)).
- **Demo pairs:** `onboard-cap`

### EST. / DOSE / CAP

- **What it is:** the loudness panel's hearing row while a sensitivity is known ([§7](#the-hearing-guard-listening-level-estimate-dose-and-cap-settings--hearing)).
- **Demo pairs:** `hearing-cap`

### CPU, peak, idle freeze and the latency line

- **What it is:** the header's readout: the latency (*~5.4 ms*, *~* while it is an estimate), the CPU load and its peak (*42% pk 97%*, the slowest 0.1 % of callbacks), xruns when the device reports them, and *OVERLOAD* with advice when it is sustained ([E45](11-enhancement-report.md#e45)). A strip fed silence for 10 s is frozen (not processed) and wakes in the same block when sound returns.
- **What it does to the sound:** nothing, unless the automatic overload response (Settings › Processing, off) steps the latency profile down.
- **Demo pairs:** none (performance; no sound of its own)

---

## 9. Control surfaces

### Global hotkeys

- **What it is:** 11 actions, all configurable in Settings › Hotkeys; the strip actions go to the *hotkey strip* (the strip of the automatic profile in front, else Game), never the strip the window happens to show ([E56](11-enhancement-report.md#e56)).

  | Action | Default | What it does |
  |---|---|---|
  | Enable / Disable | Ctrl+Alt+F | bypass of every strip |
  | Toggle Music / Gaming | Ctrl+Alt+M | the hotkey strip's mode |
  | Boost +10% / −10% | Ctrl+Alt+Up / Down | the hotkey strip's Boost |
  | Next / Previous preset | Ctrl+Alt+Right / Left | the hotkey strip's preset |
  | Focus (footsteps) | Ctrl+Alt+S | latches Footsteps at 100 % (Gaming only) until pressed again |
  | ChatMix: more chat / more game | Ctrl+Alt+PageUp / PageDown | the ChatMix balance ±0.2 |
  | Night listening | Ctrl+Alt+N | latches Night Mode Gaming's dynamics |
  | Bypass hotkey strip | Ctrl+Alt+Shift+B (was Ctrl+Alt+B) | that strip only, loudness matched |

- **Listen for:** each press changes what its row says, on the strip you play on.
- **When one does nothing:** another program may hold that combination (on the owner's PC another program holds Ctrl+Alt+B, which is why Bypass moved to Ctrl+Alt+Shift+B; a chord you saved yourself is kept). Flubsound then says so once in a notice under the header (*Hotkey not active: … could not be registered (another application may hold it, or the system reserves it)* with **Fix in Settings**; it lists every hotkey that still fails and drops one that works again), the tray menu shows *1 hotkey not active - fix…*, and Settings › Hotkeys shows the row in red ("Could not register") with **Pick a free one**, which tries a few alternatives, all with Ctrl+Alt+Shift (for Bypass: Ctrl+Alt+Shift+B, Ctrl+Alt+Shift+Y, Ctrl+Alt+Shift+F11), and keeps the first free one. Two actions on one chord, or a chord without Ctrl / Alt / Win, are named as such ("Same chord as …", "Not a valid shortcut"). Check by ear: press the new chord and listen for the change on the hotkey strip.
- **Demo pairs:** `gaming-boost-50`, `gaming-footsteps`, `night`, `chatmix`
- **Limits:** on Wayland the desktop's GlobalShortcuts portal must allow them (KDE 5.27+, GNOME 48+). Flubsound cannot tell which program holds a combination (the system does not say).

### The tray flyout

- **What it is:** a left click on the tray icon: the selected strip's Boost slider, a preset stepper, Bypass, the ChatMix slider with the voice dot, and a button to open the window.
- **Demo pairs:** `music-boost-50`, `chatmix`

### The on-screen display

- **What it is:** after every hotkey or `ctl` action, a small display at the top centre of the screen (*GAME / Boost 60%* with a level bar) for 1.2 s, faded over 0.3 s. It never takes focus or clicks (a click on it reaches the window below; on macOS it is a panel that never becomes the key window and should also show over fullscreen apps, not yet tried on a real Mac); in exclusive fullscreen it is not shown and an optional earcon (two 60 ms blips at −24 dBFS, `ctl Earcon fullscreen`) plays instead; Tournament mode hides it ([E56](11-enhancement-report.md#e56)).
- **Demo pairs:** none (feedback; no sound of its own)
- **Look for:** with a game or a text editor focused, press a Boost hotkey: the display shows and fades, and typing still goes to the game or editor; a click on the display lands in the window under it.
- **Limits (gated):** the PresentMon check with borderless games is the owner's; the macOS and Linux behaviour is checked on CI's machines only (no Mac or Linux desktop with a fullscreen game yet); Settings has no row for it yet (`flubsound-cli ctl Osd on|off`).

### `flubsound-cli ctl`

- **What it is:** runs an action in the running app from a terminal, a script or a macro key: `flubsound-cli ctl BoostUp Game` prints *Game: Boost 60%*. The 11 hotkey actions, `Boost [strip] 0-100`, `LoadPreset [strip] <uuid>`, `Show`, `Osd`, `Earcon`; `ctl --list` lists them. Exit codes: 0 ok, 1 not applied, 2 usage, 3 Flubsound is not running.
- **Demo pairs:** `gaming-boost-50`

---

## 10. Offline and the plug-in

### `flubsound-cli process` and `batch`

- **What it is:** renders WAV files offline through exactly the chain the app runs, sample-aligned: `flubsound-cli process -i song.wav -o out.wav --preset "Punchy Pop" --boost 60 --target-lufs -12`; `batch` does a folder on several threads. `--macro punch=60`, `--set key=value`, `--profile`, `--protection`, `--ceiling`, `--target-lufs` (up to 4 re-renders to within 0.3 LU).
- **What it does to the sound:** what the settings do; every pair's *Before* / *After* line in the demo index is a `process` command line (the pairs marked `+ app:` also need the app's engine).
- **Demo pairs:** `music-boost-100`

### `flubsound-cli analyze`

- **What it is:** measures a file: integrated / short-term loudness, LRA, true and sample peak, RMS, PLR; `--bands` (octave bands, what the demo index's band deltas use), `--events`, `--glitches` (clicks, dropouts, NaN, DC steps; `--band-check`, off by default, also sets aside sharp onsets under a high cut - and with them a break made ahead of one, [E53](11-enhancement-report.md#e53)), `--spatial` (IACC, DRR, diffuse-field deviation), `--focus-ild`, `--json`; and always the content reading and `Suggest` (what Smart macros would do to this file).
- **Listen for:** run it on a demo file and compare with your ears: a band delta of +2 dB at 4 kHz is a clearly brighter voice.
- **Demo pairs:** `music-clarity`, `smart-macros`

### `flubsound-cli quality` and `soak`

- **What it is:** `quality` measures a setting on pinned stimuli (THD+N, IMD, MTND, ducking, kick timing, aliasing, DC and ultrasonic energy) against the numeric targets of the regression suite ([E59](11-enhancement-report.md#e59)); `soak` runs a long seeded session with parameter automation and counts glitches (the same programme for a seed on every compiler; `--band-check` as for `analyze --glitches`).
- **Demo pairs:** none (measurements; no audio to judge)

### `flubsound-cli demo`

- **What it is:** the demo pack of [§0](#the-demo-pack-flubsound-cli-demo).
- **Demo pairs:** `music-boost-100` (and every pair of the pack)

### `flubsound-cli latency-probe`

- **What it is:** `generate` writes sweep files to play through a loopback path, `analyze` reads the recording back and reports the delay (within 0.06 samples offline) ([E42](11-enhancement-report.md#e42)).
- **Demo pairs:** none (a measurement; the owner's loopback run is the real check)

### `flubsound-cli params`, `presets`, `help` and `--version`

- **What it is:** every parameter key with its range and default; the factory presets; help per command; the version.
- **Demo pairs:** none (lists; no sound)

### In-app Export / batch

- **What it is:** preset menu (…) › *Export / batch process audio files…*: files or folders (WAV, AIFF, FLAC, Ogg Vorbis, MP3), an output folder and format, the strip's settings or any preset, a loudness target and a ceiling. A WAV export is byte-identical to `flubsound-cli process` with the same settings.
- **Demo pairs:** `music-boost-100`

### The Flubsound FX plug-in (VST3 / AU / Standalone)

- **What it is:** the same chain as one strip in a DAW or host: every parameter as a host parameter, its state saved with the project, its latency reported to the host (4.0 ms in Balanced at 48 kHz). The app's own settings (the per-ear profile, the hearing guard, the chat duck, the headset cap) are not in the plug-in.
- **Demo pairs:** `music-boost-100`

---

## 11. Settings and support

### Settings › Audio

- **What it is:** the output device profile box, the device selector (type, device, rate, buffer), the feedback-loop guard with *Allow this pair*, *Follow the system default output*, the headset enhancement answer, and LATENCY: *Automatic buffer size* and *Measure latency…* ([§1](#1-getting-sound-through-flubsound), [§7](#7-your-headset-and-your-ears), [How to measure your latency](#how-to-measure-your-latency-settings--audio)).
- **Demo pairs:** `safe-speaker-cap`, `onboard-cap`

### Settings › Correction

- **What it is:** the headphone / speaker correction for the open output ([§7](#headphone-correction-settings--correction)).
- **Demo pairs:** `device-correction`

### Settings › Processing

- **What it is:** Latency profile; automatic overload response; Protection strength; Automatic preamp (and hot programme) and Smart macros per strip; Voice chat (duck and depth, and *Neural voice cleanup on the Chat strip (experimental)* with its Status line, [§6](#neural-voice-cleanup-experimental)); Listening level (the contour, follow the system volume, reference volume, the curve).
- **Demo pairs:** `latency-profile`, `protection-normal`, `auto-preamp`, `smart-macros`, `chat-duck`, `neural-voice-cleanup`, `contour`

### Settings › Routing

- **What it is:** the per-app routing method; *Move the app's own sound away automatically* (Windows, off by default) with a line on what it does now, and the *Silent device*: *Automatic (…)* names the device it picks; Flubsound's output and the system default are greyed out ([§1](#original-also-audible-the-doubling-guard)). Then the device input: *Device input* (Automatic / Always / Off), *Input feeds strip*, and the **input map**: for each strip, the first input channel that feeds it (used only while the device input is processed: the line under the map says when it is not, for example *Device input is off: the map is not used.*). Use the map when one input carries several strips, for example a 14-channel JACK / PipeWire input, or a virtual mixer's outputs. **Fill in one after another** sets Game 1–8, Music 9–10, Chat 11–12 and System 13–14 (the order of Flubsound's Linux sinks); **Clear map** goes back to *Input feeds strip*. On Linux, Flubsound links each `flubsound_<strip>` sink's monitor to those inputs by itself (R4.6, [E48](11-enhancement-report.md#e48)).
- **Demo pairs:** none (routing; the sound is the strip's chain)
- **Limits:** the input map was tested with fakes and on Windows only; it has not run on a Linux desktop.

### Settings › Hearing

- **What it is:** the listening-level estimate, the cap, the estimated dose and the personal profile ([§7](#7-your-headset-and-your-ears)), in non-medical words.
- **Demo pairs:** `hearing-cap`, `per-ear`

### Settings › Hotkeys

- **What it is:** the switch for system-wide hotkeys, *Hotkeys act on* (the hotkey strip, Game by default), and one row per action with its chord and registration status ([§9](#global-hotkeys)). To change a chord, click it (or press Space / Return on it) and press the new combination (Esc cancels, Backspace clears it; Flubsound's own hotkeys pause meanwhile so their chords can be recorded, and any other control on the page ends the recording first; a chord another action has is refused with its name, and so is one that other applications or the system use, such as Alt+F4, F5 alone or Ctrl+C: *Not taken: …*). Right-click or press Shift+F10 on it to type one instead (for Win / Super chords, or a Ctrl+letter chord you want on purpose). A row in red is not active: the reason is next to it and under the rows, and **Pick a free one** picks a free combination. The reset button puts the default back.
- **Demo pairs:** `gaming-boost-50`

### Settings › General

- **What it is:** *Start Flubsound Pro when I sign in*, *Start minimised*, *Close button keeps Flubsound running in the tray*, UI scale (75–200 %) and theme (standard or high contrast), the settings and preset folders.
- **Demo pairs:** none (no sound)

### Settings › Diagnostics and Updates

- **What it is:** the log folder (device changes, errors, glitches and overloads; no audio), crash reports, **Export diagnostics** (one zip with your home folder, login and computer name replaced), and the update check: off by default, notify only (it never downloads or installs), Stable or Beta, *Check now* ([E54](11-enhancement-report.md#e54)).
- **Demo pairs:** none (no sound)
- **Limits (gated):** the first check against a real GitHub release (none exists yet); signed installers wait for certificates.

---

## 12. By-ear checklist

One row per feature, for the owner to fill in and return. Play the pair (in the pack's `index.txt` order), then try the same control in the app on your own music or game. *Pass* means you heard what the row says, at matched loudness where it applies. Rows marked † describe features new in batch 5 that no listener has heard yet: they need your ear most.

| # | Feature | Demo pair | You should hear | Pass / fail | Notes |
|---|---|---|---|---|---|
| 1 | Boost 50 (Music) | `music-boost-50` | clearer and wider, hardly louder | | |
| 2 | Boost 100 (Music) | `music-boost-100` | denser drums, more bass, no harshness or pumping | | |
| 3 | Boost 50 (Gaming) | `gaming-boost-50` | detail and direction first, hardly louder | | |
| 4 | Boost 100 (Gaming) | `gaming-boost-100` | steps and placement clearer; no pumping after blasts | | |
| 5 | Punch † | `music-punch` | a click and thump at each hit's start, no louder mix | | |
| 6 | Punch at Boost 100 † | `music-punch-boost-100` | the same on both sides: no ticks at the hits' starts (Punch's attack is off from Boost 70 %) | | |
| 7 | Width | `music-width` | pad and hats wider; nothing lost in mono | | |
| 8 | Clarity | `music-clarity` | voice and hats forward, less boxy low mids | | |
| 9 | Loudness (level) | `music-loudness` | louder and denser; drums jump less | | |
| 10 | Warmth | `music-warmth` | darker, rounder, fuller body; not muffled | | |
| 11 | Tape grit | `tape-grit` | more grit and bass, less soft top | | |
| 12 | Footsteps | `gaming-footsteps` | steps stand out when they happen, ambience unchanged | | |
| 13 | Positional | `gaming-positional` | steps and gunshots easier to place | | |
| 14 | Impact † | `gaming-impact` | bigger thump on blasts, not on the rumble | | |
| 15 | Impact at Boost 100 † | `gaming-impact-boost-100` | steps after a blast still audible | | |
| 16 | Detail | `gaming-detail` | quiet sounds closer to the loud ones | | |
| 17 | Voice & Score | `gaming-voice-score` | voice line and score through the effects | | |
| 18 | Smart macros † | `smart-macros` | the loud master less squashed and distorted | | |
| 19 | Dynamic Range 10 LU (level) | `startle-guard` | blasts no longer jump out, steps stay | | |
| 20 | Smoothness | `smoothness` | softer "s" sounds, vowels still clear | | |
| 21 | Attack Low | `attack-low` | firmer kick start only | | |
| 22 | Attack High | `attack-high` | sharper hats and snare crack only | | |
| 23 | Relative presence | `relative-presence` | the quiet master less bright at 2–5 kHz | | |
| 24 | Noise gate | `noise-gate` | less hiss in the pauses, voice natural | | |
| 25 | EQ band | `eq-bell` | the 4 kHz region forward | | |
| 26 | Dynamic EQ | `dynamic-eq` | loud vowels and snares less edgy | | |
| 27 | Bass boost | `bass-boost` | more thump and bass-line weight | | |
| 28 | Harmonic bass | `bass-harmonics` | the bass deeper on small speakers | | |
| 29 | Tighten | `bass-tighten` | drier, tighter kick | | |
| 30 | Saturation (Tube) | `saturation` | denser, rounder, slightly gritty | | |
| 31 | Crossfeed (headphones) | `crossfeed` | less inside the head, centre unchanged | | |
| 32 | Crossfeed Meier | `crossfeed-meier` | clearer sides than Bs2b | | |
| 33 | Crossfeed Mono-safe | `crossfeed-mono-safe` | centre exactly as before | | |
| 34 | Virtualiser (headphones) | `virtualiser` | steps circle outside the head | | |
| 35 | Enhanced renderer † | `enhanced-renderer` | front and behind easier to tell | | |
| 36 | Front/Back 100 % † | `virt-front-back` | behind more clearly behind | | |
| 37 | Compressor | `compressor` | flatter dynamics, drums less snappy | | |
| 38 | Maximizer Transparent | `max-style-transparent` | cleanest loudness, drums keep shape | | |
| 39 | Maximizer Punchy | `max-style-punchy` | harder drum attacks at the same loudness | | |
| 40 | Maximizer Aggressive | `max-style-aggressive` | loud and dense with grit | | |
| 41 | Maximizer Safe | `max-style-safe` | fewest artefacts, softer drums | | |
| 42 | Loudness contour | `contour` | bass audible at a quiet volume | | |
| 43 | Automatic preamp | `auto-preamp` | less pumping at the same loudness | | |
| 44 | Latency profile | `latency-profile` | hardly any difference (A/B/X) | | |
| 45 | Protection Normal | `protection-normal` | less grit and harshness | | |
| 46 | Night (level) | `night` | quiet parts up, blasts held | | |
| 47 | Late Night Low Volume (level) | `preset-late-night` | everything audible when quiet | | |
| 48 | Podcast & Voice (level) | `preset-podcast-voice` | phrases even, consonants clear | | |
| 49 | Voice Chat (level) | `preset-voice-chat` | quiet and loud talkers even | | |
| 50 | Rock & Metal | `preset-rock-metal` | less box, more bite, less fizz | | |
| 51 | Orchestral & Film | `preset-orchestral-film` | deeper lows, softer 2–4 kHz, full dynamics | | |
| 52 | Acoustic & Singer-Songwriter | `preset-acoustic-singer-songwriter` | closer, rounder voice | | |
| 53 | R&B & Vocal | `preset-rnb-vocal` | smooth deep lows, voice forward | | |
| 54 | Electronic & Ambient | `preset-electronic-ambient` | deeper sub, wider pad | | |
| 55 | Synthwave | `preset-synthwave` | deeper sub with a tight kick, leads forward, wider pad, no harshness | | |
| 56 | ChatMix (level) | `chatmix` | game quieter, voice unchanged | | |
| 57 | Chat duck (level) | `chat-duck` | voice easier to follow while it talks | | |
| 58 | Headset enhancement cap | `onboard-cap` | smaller cue lift (the headset adds its own) | | |
| 59 | Safe speaker bass cap | `safe-speaker-cap` | much less low end | | |
| 60 | Headphone correction | `device-correction` | the example curve's tone | | |
| 61 | Listening-level cap (level) | `hearing-cap` | loud passages glide down, tone unchanged | | |
| 62 | Personal profile | `per-ear` | right ear brighter | | |
| 63 | Doubling fix (app, Windows) | — | the captured app heard once, no phasing | | confirmed 2026-09-29 |
| 64 | Headset matched (app) | — | Settings › Audio names the Turtle Beach series | | |
| 65 | Re-plug / sleep (app) | — | the dongle in another port keeps its settings | | |
| 66 | Neural voice cleanup (experimental) † | `neural-voice-cleanup` | fan, hum and typing lower between words; the voice whole, not watery | | |

---

## 13. What cannot be judged by ear yet

These are measured or built, but the judgement needs something this computer does not have. In plain words:

- **A listening panel.** Every "you should hear" above is a prediction from measurements. A panel of listeners is needed for: the voicing of the five genre presets and the crossfeed presets (E14 step 4, E12 Phase B), whether the governor's budgets are inaudible, Smart macros against static macros on real masters (E34), the Enhanced renderer as the default (E28), the loudness contour against Late Night (E32), and the 30-minute fatigue session (E07).
- **The device lab.** Real headset sensitivities (so the hearing guard can be on by default) and the estimate's accuracy (±3 dB on 5 headsets), hardware ids for headset matching, what Superhuman Hearing and on-board EQ actually do to the response, and 20 power cycles of a dongle (E16, E32, E51).
- **The owner's Windows 11 PC with the Turtle Beach headset.** The dongle re-plug with its correction following it and *Follow the system default output* (E51), the system-volume reads for the contour and the hearing guard (E32), the anti-cheat service query and a 1 h trace in a game (E55), the OSD with PresentMon in borderless games (E56), SteelSeries Sonar's ChatMix against Flubsound's with Discord (E22), the update check against a real release (E54), and the loopback latency measurement (E42).
- **CI.** CI runs again since 2026-09-30 and has been green on every OS since run 37572308549 (2026-10-07, batches 3–5; the E56 runs that followed were all 11 jobs green, [E56](11-enhancement-report.md#e56)). The work merged on 2026-10-07 (the latency measurement, the real-device soak, hotkey conflicts, the routing move-away and input map, the neural voice cleanup) ran green on all 12 jobs in run 37649158870. The five items merged on 2026-10-08 (the preset render diff on CI, the Enhanced renderer's comb fix, the duck's room for a loud teammate, the bypass limiter's smooth take-over with the detector's band check, the woken neural worker) each ran all 12 jobs green on its own branch before the merge, and the merged code in run 37741568844. Whether pluginval at strictness 10 on Windows and macOS and the Windows update-check path ran is to be read from those runs' logs.
- **Certificates.** Signed installers and a signed update feed (E54).
- **Owner decisions.** Relative presence as the default (re-voices every preset that lifts presence), the onset-flux key in the gaming mode bands (re-voices the gaming presets), Night Mode's compressor attack 3 → 1 ms, the five genre presets' voicing, the per-ear profile's headroom reservation (a +12 dB one-ear high-frequency profile turns both ears down about 9 dB), Smart playing Loudness 60 about 1.1 LU quieter on limited masters, the duck's room floor for a loud teammate (−6 dB, provisional, or −9 dB; E22) and a soft attack for the Positional focus's guard (it re-voices every render with Positional above 0; E53).
- **Known gaps you may hear.** A loud teammate (−14 LUFS) still loses 0.45 dB under explosions with the duck on, at the room's provisional −6 dB floor (0.18 dB at −9 dB, for 2.7 dB more pull-down of the explosions' peaks: an owner decision; E22); Music Boost 100 + Clarity 100 at Normal is 2.00 dB brighter on quiet than on loud material against a 1.5 dB target (E07); Classical & Jazz loses 1.42 LU of dynamic range on a hot master (E14); the soak test still finds rare clicks at about −43 to −50 dB: the maximizer at full Boost, a one-sample step of the Positional focus's guard (its instant attack; an owner decision) and two after a bank switch and a dynamic-EQ switch, not yet triaged (E53; since 2026-10-08 the bypassed sound no longer clicks at high input gain); the bass engine's default headroom protection holds most of a bass boost back on loud masters (§4, Bass Engine card).
