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
  entry per process: pid from `pipewire.sec.pid` (the host pid from the
  socket credentials) or else `application.process.id` (the client's own
  `getpid()`, which is sandbox-local for Flatpak/Snap apps), binary,
  application name, the current sink name, and
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
provides the server side. The calls block for roughly 5–30 ms, so the app
runs them on its routing worker thread (`AppRouting`, every 2 s while routes
exist), never on the message or audio thread.

**Flatpak.** The sandbox has no `pactl`. The Flatpak build has to bundle
`pactl` (with `--socket=pulseaudio`) or replace these calls with libpulse or
libpipewire.

## Global hotkeys

Implemented for X11 sessions (`LinuxGlobalHotkeys` in
`app/Source/platform/PlatformServices_linux.cpp`); not yet for Wayland.

- **X11.** `XGrabKey` on the root window, once per chord combined with each
  of {none, CapsLock, NumLock, both}, so the lock keys do not defeat the
  shortcut. libX11 is loaded at run time with `dlopen("libX11.so.6")`, so
  the app has no link dependency on it; the X11 headers are needed at build
  time only, and without the headers or the library the service reports
  itself unsupported. The grabs live on a private `Display` connection
  served by the service's own event thread (`poll` on the X connection plus
  a wake pipe); callbacks run on that thread and `HotkeyManager` moves them
  to the message thread. A chord another X client already grabbed fails with
  `BadAccess`, caught by a temporary `XSetErrorHandler`, and
  `registerHotkey` returns false. `XkbSetDetectableAutoRepeat` and a
  per-chord "down" flag make a held key fire once. Bare keys, F-keys
  included, are refused because they would steal normal typing.
- **Wayland.** Grabbing keys is forbidden by design (an X grab through
  XWayland only sees keys while an XWayland window has focus). In a Wayland
  session (`XDG_SESSION_TYPE=wayland` or `WAYLAND_DISPLAY` set), and with no
  `DISPLAY`, `GlobalHotkeys::isSupported() == false` and the UI suggests
  binding Flubsound's actions in the desktop's keyboard settings. The proper
  API is the **xdg-desktop-portal GlobalShortcuts** interface
  (`org.freedesktop.portal.GlobalShortcuts`: `CreateSession`,
  `BindShortcuts`, `Activated` signal); it is roadmap. The compositor shows
  a confirmation dialog and the user may rebind keys. It is available in KDE
  Plasma 5.27+, GNOME 48+ and Hyprland.

Test: `Platform: X11 global hotkeys fire once per press, refuse a chord
another client holds, and release on unregister` in
`tests/test_platform_linux.cpp` synthesises key events with XTest. It is
skipped without an X display or `libXtst`; CI runs it under `xvfb-run` in the
`sanitizers` job.

## Real-time scheduling

`SystemTuning::promoteAudioThread()` tries `SCHED_FIFO` with priority 20,
the same ceiling rtkit grants. It adds `SCHED_RESET_ON_FORK` so that
children such as `pactl` never inherit real-time priority. If that fails, it
retries at the user's `RLIMIT_RTPRIO`, then silently stays at
`SCHED_OTHER`. Without `CAP_SYS_NICE` the kernel does not let a thread clear
`SCHED_RESET_ON_FORK` again, so `revertAudioThread()` restores the old
policy with the flag kept set, which is harmless for `SCHED_OTHER`. Ways to grant RT rights:

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

A per-stream `node.latency = 256/48000` request for Flubsound's own streams
would be the targeted alternative; the engine does not set it yet (it uses
JUCE's ALSA / JACK backends), so this is roadmap.

## Headset profiles on Linux

`AudioEndpoints::queryOutputTransport()` returns *unknown* on Linux: JUCE's
ALSA / JACK device names do not map one-to-one to PipeWire / Pulse sinks, so
the PipeWire `device.bus` property is not queried. Headset profiles therefore
detect Bluetooth and hands-free outputs from the device name and format
(`flub::device::detectConnection`, see `docs/10-headset-compatibility.md`).

## Roadmap

- **Native PipeWire filter node.** Host `flub_core` in a `pw_filter` with one
  input port group per strip. This removes the JUCE device layer and the
  monitor hop, and lets WirePlumber manage the links.
- libpulse or libpipewire routing instead of shelling out to `pactl`, for
  Flatpak and to receive change events instead of polling.
- xdg-desktop-portal GlobalShortcuts (global hotkeys in Wayland sessions).
- RealtimeKit for the audio thread, `node.latency` for Flubsound's streams,
  and the output's `device.bus` for headset connection detection.
