# Flubsound on Linux: PipeWire / PulseAudio integration

On Linux the sound server already has everything per-app processing needs,
so **no kernel driver is required**:

```
 game  ──► [flubsound_game   null sink] ──► flubsound_game.monitor   ─┐
 spotify ─► [flubsound_music  null sink] ──► flubsound_music.monitor  ─┤  Flubsound engine
 discord ─► [flubsound_chat   null sink] ──► flubsound_chat.monitor   ─┤  (MixEngine strips)
 others ──► [flubsound_system null sink] ──► flubsound_system.monitor ─┘        │
                                                                                 ▼
                                                              real output (headphones)
```

- Each strip gets a **null sink**. Applications play into it like into any
  output device.
- The engine records each sink's **monitor**, processes it and plays the
  mix on the real device.
- Routing an application means moving its stream (a PulseAudio
  *sink-input*, a PipeWire stream node) to the strip's sink. PipeWire's
  session manager, WirePlumber, remembers the choice per application.
- Per-process loopback capture is therefore not needed:
  `ProcessLoopbackCapture::isSupported()` is `false` on Linux.

This works the same on **PipeWire** (with `pipewire-pulse`, the default on
current Fedora, Ubuntu, Debian, Arch and SteamOS) and on classic
**PulseAudio**.

## Files

| File | Purpose |
|---|---|
| `flubsound-pipewire-setup.sh` | Creates or removes the four sinks at run time via `pactl load-module module-null-sink` (idempotent). Also has `status`. |
| `pipewire/pipewire.conf.d/90-flubsound-sinks.conf` | Same sinks, created by the PipeWire daemon at every start (persistent). Use this *or* the script. |
| `pipewire/pipewire-pulse.conf.d/90-flubsound-app-routing.conf` | Example per-app rules: sets `target.object` so WirePlumber links, for example, `cs2` to `flubsound_game` from the first sound on. |

```sh
# one-off (until the sound server restarts)
platform/linux/flubsound-pipewire-setup.sh install
platform/linux/flubsound-pipewire-setup.sh status
platform/linux/flubsound-pipewire-setup.sh remove

# persistent (PipeWire)
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp platform/linux/pipewire/pipewire.conf.d/90-flubsound-sinks.conf ~/.config/pipewire/pipewire.conf.d/
systemctl --user restart pipewire pipewire-pulse wireplumber
```

With classic PulseAudio, persistence means appending the output of
`flubsound-pipewire-setup.sh print-pa-config` to `~/.config/pulse/default.pa`
(after `.include /etc/pulse/default.pa`).

Sink names and properties:

| Sink | Description | Channels |
|---|---|---|
| `flubsound_game` | Flubsound Game | 7.1 (`FL FR FC LFE RL RR SL SR`); games render surround, the Game strip's HRTF folds it to binaural |
| `flubsound_music` | Flubsound Music | stereo |
| `flubsound_chat` | Flubsound Chat | stereo |
| `flubsound_system` | Flubsound System | stereo; can be made the default output so "everything else" is processed too |

## How the app does per-app routing

`app/Source/platform/PlatformServices_linux.cpp` implements
`AppAudioRouter`:

- **`enumerateSessions()`** runs `pactl --format=json list sink-inputs` and
  `… list sinks` and parses the output with `flub::json`. It returns one
  entry per process: pid from `application.process.id` (or
  `pipewire.sec.pid`), binary, application name, the current sink name, and
  whether the stream is playing (`corked == false`). Flubsound's own streams
  and streams without a pid are skipped.
- **`setAppEndpoint(pid, sink)`** runs `pactl move-sink-input <id> '<sink>'`
  for every stream of that pid. An empty sink means `@DEFAULT_SINK@`. The
  sink argument passes a strict whitelist (`[A-Za-z0-9_.:@+-]`, no leading
  `-`, max 255 characters) and is single-quoted as well. Nothing unsanitised
  ever reaches `popen`.
- **`openSystemRoutingSettings()`** starts `pavucontrol --tab=1` (Playback),
  or failing that `pwvucontrol`, GNOME Settings › Sound, or KDE's audio KCM.

`pactl --format=json` needs `pactl` 16 or newer. That covers
pulseaudio-utils 16+ and every current distribution; pipewire-pulse only
provides the server side. The calls block for a few milliseconds, so the UI
calls them off the message thread.

**Flatpak.** The sandbox has no `pactl`. The Flatpak build has to bundle
`pactl` (with `--socket=pulseaudio`) or replace these calls with libpulse or
libpipewire.

## Global hotkeys

Not implemented on Linux yet (`GlobalHotkeys::isSupported() == false`).

- Under Wayland, grabbing keys is forbidden by design. The proper API is the
  **xdg-desktop-portal GlobalShortcuts** interface
  (`org.freedesktop.portal.GlobalShortcuts`: `CreateSession`,
  `BindShortcuts`, `Activated` signal). The compositor shows a confirmation
  dialog and the user may rebind keys. It is available in KDE Plasma 5.27+,
  GNOME 48+ and Hyprland.
- Under X11 the implementation would be `XGrabKey` on the root window, once
  per chord combined with the NumLock and CapsLock masks.

Until then the UI suggests binding Flubsound's actions in the desktop's
keyboard settings.

## Real-time scheduling

`SystemTuning::promoteAudioThread()` tries `SCHED_FIFO` with priority 20,
the same ceiling rtkit grants. It adds `SCHED_RESET_ON_FORK` so that
children such as `pactl` never inherit real-time priority. If that fails, it
retries at the user's `RLIMIT_RTPRIO`, then silently stays at
`SCHED_OTHER`. Ways to grant RT rights:

- the distribution's `audio` or `realtime` group with `rtprio` limits
  (`/etc/security/limits.d/`), or
- RealtimeKit over D-Bus (`org.freedesktop.RealtimeKit1.MakeThreadRealtime`).
  This is not wired up yet, to avoid a D-Bus dependency.

When the engine runs as a JACK or PipeWire client (JUCE's JACK backend on
`pipewire-jack`), the server calls the process callback on its own RT thread,
which is the recommended setup.

`SystemTuning::disablePowerThrottling()` is a no-op on Linux. CPU frequency
policy is system-wide (cpufreq governor, power-profiles-daemon).

## Latency

PipeWire processes the whole graph in cycles of one *quantum* (default
1024/48000 = 21 ms on many distributions; games and pro-audio setups use
256/48000 = 5.3 ms or less). The null sink and its monitor add at most one
quantum. Force a smaller quantum for gaming with

```sh
pw-metadata -n settings 0 clock.force-quantum 256
```

or have the engine request `node.latency = 256/48000` for its streams.

## Roadmap

- **Native PipeWire filter node.** Host `flub_core` in a `pw_filter` with one
  input port group per strip. This removes the JUCE device layer and the
  monitor hop, and lets WirePlumber manage the links.
- libpulse or libpipewire routing instead of shelling out to `pactl`, for
  Flatpak and to receive change events instead of polling.
- xdg-desktop-portal GlobalShortcuts.
