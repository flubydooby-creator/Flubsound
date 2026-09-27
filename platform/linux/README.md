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

Implemented for X11 sessions (`LinuxGlobalHotkeys`) and for Wayland
sessions through the xdg-desktop-portal (`PortalGlobalHotkeys`), both in
`app/Source/platform/PlatformServices_linux.cpp`. `GlobalHotkeys::create()`
picks the portal in a Wayland session (`XDG_SESSION_TYPE=wayland` or
`WAYLAND_DISPLAY` set, even when XWayland also provides a `DISPLAY`) and
X11 otherwise.

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
  `registerHotkey` returns false. Every registration is also reported to
  the binding listener (`Registered` / `Unavailable`) before it returns.
  X11 has no list of shortcuts, so the action's description is not used.
  `XkbSetDetectableAutoRepeat` and a
  per-chord "down" flag make a held key fire once. Bare keys, F-keys
  included, are refused because they would steal normal typing.
- **Wayland.** Grabbing keys is forbidden by design (an X grab through
  XWayland only sees keys while an XWayland window has focus). The
  sanctioned API is the **xdg-desktop-portal GlobalShortcuts** interface
  (`org.freedesktop.portal.GlobalShortcuts` version 1 on
  `org.freedesktop.portal.Desktop`), available in KDE Plasma 5.27+, GNOME
  48+ and Hyprland:
  - libdbus-1 is loaded at run time with `dlopen("libdbus-1.so.3")`. The
    few functions and the two caller-allocated structs are declared in the
    source from libdbus's stable ABI, so neither the headers
    (`libdbus-1-dev`) nor a link dependency is needed, and one declaration
    path is compiled and tested everywhere.
  - The service opens a private connection to the session bus
    (`$DBUS_SESSION_BUS_ADDRESS`, else `$XDG_RUNTIME_DIR/bus`; never X11
    autolaunch) and reads the interface's `version` property. When the
    portal or the interface is missing, `isSupported() == false` and the UI
    suggests binding Flubsound's actions in the desktop's keyboard settings.
    Otherwise its own thread serves the connection (`poll` on the bus fd
    plus a wake pipe) and calls the callbacks, once per `Activated` signal.
    `Deactivated` (key release) is ignored. Signals count only when they come
    from the portal's unique bus name and name the current session.
  - `CreateSession`, then, once its `Response` signal carries the
    `session_handle`, `BindShortcuts` with one entry per chord: id
    `flubsound-<action>`, description `Flubsound Pro: <action name>`
    (the description `HotkeyManager` passes, e.g. "Boost +10%", with
    invalid UTF-8 replaced by `?` because libdbus aborts on it;
    `Flubsound Pro: <chord>` when there is none) and a
    `preferred_trigger` in the XDG shortcuts format: modifiers `CTRL`,
    `ALT`, `SHIFT`, `LOGO`, then the xkb keysym name of the unshifted key,
    for example `CTRL+ALT+Up`, `CTRL+SHIFT+m`, `CTRL+Page_Up` or `F13`.
    The desktop may show a dialog; the user can pick another key or
    decline.
  - Binding is asynchronous, so `registerHotkey()` returns true when the
    chord could be requested (valid chord, portal present). The outcome goes
    to the binding listener (`GlobalHotkeys::setBindingListener`) from the
    service's thread once the portal answers, and `HotkeyManager` shows it
    per action on the Hotkeys page: an id in the `BindShortcuts` response is
    `Registered`, or `Reassigned` when its `trigger_description` names
    another key (compared by modifiers and key with common aliases such as
    `PgUp` / `Page_Up` and `Meta` / `LOGO`; English names only, so a
    localised description counts as another key and is shown verbatim). An
    id missing from the response, or a response code 1 (the user cancelled)
    or 2, is `Declined`; a D-Bus error is `Unavailable`. A batch that needs
    no new binding repeats the previous outcome, a session the desktop
    closes later turns its shortcuts `Declined`, and a portal that goes away
    turns them `Unavailable`. Refusals and errors are also logged to stderr.
  - Rebinding: the interface has no "unbind", and a session's shortcuts are
    bound once. A changed set is therefore bound in a new session and the
    old one is closed (`Session.Close`). Desktops remember the user's choice
    per application and shortcut id, so known ids are not asked about again.
    Changes within 50 ms are coalesced, because `HotkeyManager::registerAll()`
    unregisters everything and re-registers each action; when the result is
    the set already bound, nothing is sent at all. If the portal restarts,
    the set is bound again with the new instance.
  - `parent_window` is empty (the interface gives the service no window
    handle), and the app has no installed `.desktop` file yet, so desktops
    identify it by its process (for example its systemd scope) rather than
    by an application id. How each desktop treats such an unregistered
    application is untested.

Tests in `tests/test_platform_linux.cpp`:

- `Platform: X11 global hotkeys fire once per press, refuse a chord another
  client holds, and release on unregister` synthesises key events with
  XTest. It is skipped without an X display or `libXtst`; CI runs it under
  `xvfb-run` in the `sanitizers` job.
- `Platform: Wayland global hotkeys bind through the GlobalShortcuts portal,
  fire once per activation and rebind in a new session` starts a private
  `dbus-daemon` and a mock portal implemented in the test with libdbus. It
  checks the ids, descriptions and triggers `BindShortcuts` receives, that
  one `Activated` fires the right callback exactly once and that signals from
  another client or a closed session are ignored. It also checks that
  unregistering or changing a chord re-creates the session and that
  re-registering the same set does not. `Platform: Wayland portal shortcuts
  carry the action's description and report registered, reassigned and
  declined bindings` checks the descriptions `BindShortcuts` receives and
  the binding listener's results: synchronous refusals on the calling
  thread, then from the service thread a shortcut bound as requested (also
  when the desktop words the trigger its own way), one bound to another key,
  one left out, the same outcome again for an unchanged set, a cancelled
  dialog, a repaired non-UTF-8 description and a session the desktop
  closes. `Platform: Wayland global hotkeys are unsupported without a
  GlobalShortcuts portal` covers a bus without a portal, a portal without
  the interface and no bus at all. These are skipped without `dbus-daemon`
  or libdbus-1; the CI `sanitizers` job installs `dbus`. `Platform: portal
  trigger descriptions compare by modifiers and key, and shortcut
  descriptions are made valid UTF-8` needs neither.

## Start at sign-in

*Settings › General › Start Flubsound Pro when I sign in* writes an XDG
autostart entry (`LinuxAutoStart` in
`app/Source/platform/PlatformServices_linux.cpp`):
`$XDG_CONFIG_HOME/autostart/flubsound-pro.desktop`, or
`~/.config/autostart/` when the variable is unset, empty or relative. GNOME,
KDE Plasma, Xfce, Cinnamon, MATE and LXQt honour it; bare window managers
need a helper such as `dex`.

- The entry has `Type=Application`, `Name`, `Exec`, `Terminal=false` and
  `X-GNOME-Autostart-enabled=true`. `Exec` is the double-quoted absolute path
  of the executable (`$APPIMAGE` when run from an AppImage), escaped as the
  Desktop Entry spec requires: `"`, `` ` ``, `$` and `\` get a backslash
  inside the quotes, every backslash is then doubled by the string-value
  rule, and `%` becomes `%%`. Paths that are not valid UTF-8 or contain
  control characters are refused with an error. No argument is added: the app
  has no command-line switch for starting minimised; the *Start minimised*
  setting applies instead. GLib checks that the program exists before
  expanding `%%`, so GNOME skips an entry whose path contains `%`.
- The file is written to a temporary sibling, `fsync`ed and renamed over the
  entry; new folders get mode 0700. Switching the option off deletes the file.
- The switch shows the file's real state each time the page opens: a missing
  file, `Hidden=true` or `X-GNOME-Autostart-enabled=false` (how desktop
  start-up settings switch an entry off) all read as off.

Tests: the `Platform: XDG autostart ...` cases in
`tests/test_platform_linux.cpp` point `XDG_CONFIG_HOME` / `HOME` at a
temporary folder.

## Foreground application (automatic profiles)

Automatic profiles (`docs/06-gui.md` §8.1) need to know which application is
in front. `LinuxForegroundApp` in `app/Source/platform/PlatformServices_linux.cpp`
works as follows:

- **X11 sessions.** It reads the root window's `_NET_ACTIVE_WINDOW`, then that
  window's `_NET_WM_PID`, then `/proc/<pid>/exe`.
  - libX11 is loaded with `dlopen`: the same table as the hotkeys, on a
    private display connection.
  - Every EWMH window manager publishes `_NET_ACTIVE_WINDOW` (GNOME/Xorg,
    KDE/X11, Xfce, Cinnamon, MATE, i3, ...). GTK, Qt, SDL, Wine and JUCE
    clients set `_NET_WM_PID`. A window without it gives no answer, and the
    current profile is held.
  - The kernel resolves symlinks in `/proc/<pid>/exe`. A program started
    through `/bin/foo` therefore reports its real path (for example
    `/usr/bin/foo`), and a multi-call binary reports that binary.
  - Wine / Proton programs run inside a `wine[64][-preloader]` loader. They
    are reported by their Windows executable (argv[0], e.g.
    `C:\Games\CS2\cs2.exe`), so a rule written for `cs2.exe` matches on
    Linux too.
  - Another user's process (unreadable `exe`) falls back to `comm`.
  - A window destroyed between the two property reads raises `BadWindow`.
    A temporary error handler, for this connection only, swallows it.
  - The description is cached while the same window and process stay in
    front, so a 2 Hz poll is two property round trips.
- **Wayland sessions** (`XDG_SESSION_TYPE=wayland` or `WAYLAND_DISPLAY`
  set). Unsupported: Wayland does not let applications see which window is in
  the foreground, and XWayland only knows X clients. The routing panel shows
  the reason and disables adding rules. Without libX11 or an X display, the
  reason given says so.

Tests in `tests/test_platform_linux.cpp`:

- `Platform: foreground process names ...` covers the Wine / `/proc`
  naming.
- `Platform: foreground application detection is unsupported under Wayland
  ...` covers the unsupported cases.
- `Platform: X11 foreground app follows _NET_ACTIVE_WINDOW and _NET_WM_PID
  ...` needs a bare X server (CI: `xvfb-run -a`). The test creates windows,
  sets their `_NET_WM_PID` and the root's `_NET_ACTIVE_WINDOW` itself. It
  compares the reported path with the canonical path of the spawned binary.
  It is skipped without a display, or when a window manager is running.

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
  This is not wired up yet; the run-time libdbus-1 loading used for Wayland
  hotkeys could carry it without a link dependency.

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
- Wayland hotkeys: run on real desktops (so far tested only against a mock
  portal), pass a `parent_window` and ship a `.desktop` file so the portal
  knows the application id.
- RealtimeKit for the audio thread, `node.latency` for Flubsound's streams,
  and the output's `device.bus` for headset connection detection.
