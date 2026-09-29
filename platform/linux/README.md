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
- **`connectEndpointInputs()`** links each strip sink's monitor to the
  engine's input with `pw-dump` / `pw-link` (see *Linking the sinks to the
  engine* below).
- **`openSystemRoutingSettings()`** starts `pavucontrol --tab=1` (Playback),
  or failing that `pwvucontrol`, GNOME Settings › Sound, or KDE's audio KCM.

`pactl --format=json` needs `pactl` 16 or newer. That covers
pulseaudio-utils 16+ and every current distribution; pipewire-pulse only
provides the server side. The calls block for roughly 5–30 ms, so the app
runs them on its routing worker thread (`AppRouting`, every 2 s while routes
exist), never on the message or audio thread.

**Flatpak.** The sandbox has no `pactl`, `pw-dump` or `pw-link`. The Flatpak
build has to bundle them (with `--socket=pulseaudio` and access to the
PipeWire socket) or replace these calls with libpulse or libpipewire.

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
  per-chord "down" flag make a held key fire once; the key's release clears
  the flag whatever modifiers are still held, so letting go of Ctrl/Alt
  before the key does not swallow the next press. Bare keys, F-keys
  included, are refused because they would steal normal typing, and so is
  a chord another action already uses (X would accept the second grab from
  the same client and one press would run both actions; Windows refuses it
  too). Registering and unregistering wake the event thread after their
  `XSync`, which can read a pending key event into Xlib's queue.
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
    autolaunch). Its own thread serves the connection (`poll` on the bus fd
    plus a wake pipe) and asks for the interface's `version` property
    without blocking. The constructor, on the message thread, waits for
    that answer for at most 250 ms: a running portal answers at once, and
    so does D-Bus when there is none, but a portal that D-Bus is still
    starting at login can take seconds. Until the answer the service counts
    as supported and registrations wait for it. When the portal or the
    interface is missing, `isSupported() == false` and the UI suggests
    binding Flubsound's actions in the desktop's keyboard settings; a portal
    that appears on the bus later (`NameOwnerChanged`) is asked again and
    then used. A probe that times out or whose D-Bus activation fails is
    retried after 2 s, doubling up to 60 s, with the requested shortcuts
    reported `Unavailable` meanwhile and bound once the portal answers. If
    the session bus connection is lost, every shortcut is reported
    `Unavailable` and the service turns unsupported, so later registrations
    are refused at once instead of waiting forever. The service thread
    calls the callbacks, once per `Activated` signal.
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
    another key. Triggers are compared by modifiers and key with common
    aliases such as `PgUp` / `Page_Up` and `Meta` / `LOGO`, the modifier and
    key names Qt (KDE) shows in German, French, Spanish and Brazilian
    Portuguese (`Strg+Umschalt+Bild auf`, `Ctrl+Maj+Haut`,
    `Ctrl+Mayús+Arriba`, `Ctrl+Alt+Cima`), and GNOME's form, GTK
    accelerators inside a localised sentence (`Press <Control><Alt>Up`,
    `Press <Control><Alt>Up or <Super>u`); a description listing several
    triggers matches when one of them is the requested one. A description
    still not understood is shown verbatim as `Reassigned` ("Bound by the
    desktop as …"), never as `Registered`. An id without a
    `trigger_description` is `Registered`, except under GNOME
    (`XDG_CURRENT_DESKTOP`), which leaves it out when the user removed the
    key: `Declined` there. An id missing from the response, or a response
    code 1 (the user cancelled) or 2, is `Declined`; a D-Bus error is
    `Unavailable`. A batch that needs no new binding repeats the previous
    outcome, a session the desktop closes later turns its shortcuts
    `Declined` (also while its `BindShortcuts` is still waiting for the
    user; that late answer is ignored), and a portal that goes away turns
    them `Unavailable`. An answer to a batch that the hotkey settings have
    changed since is not reported, since it is not the outcome of the
    current chords; the next batch's is. Refusals and errors are also logged
    to stderr.
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
  XTest. It also releases the modifiers before the key and presses again,
  and registers the same chord for a second action. It clears
  `WAYLAND_DISPLAY` and `XDG_SESSION_TYPE`, so on a Wayland desktop it uses
  XWayland instead of the desktop's real portal. It is skipped without an X
  display or `libXtst`; CI runs it under `xvfb-run` in the `sanitizers`
  job. Its waits are 10 s hang guards, not timing assertions.
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
  the interface and no bus at all. `Platform: Wayland portal probe does
  not block start-up: a portal that answers late or appears later is used`,
  `Platform: Wayland portal: after the session bus is lost, shortcuts are
  reported unavailable and new requests are refused`, `Platform: Wayland
  portal: an answer for a set changed since, or for a session the desktop
  closed, is not reported as the outcome` and `Platform: Wayland portal:
  GNOME's and localised descriptions of the requested key count as
  registered, a GNOME shortcut without a key does not` cover the start-up
  probe, bus loss, stale answers and trigger descriptions (German
  `Strg+Alt+Hoch` for `CTRL+ALT+Up` is `Registered`). These are skipped
  without `dbus-daemon` or libdbus-1, and fail (not skip) when
  `dbus-daemon` is installed but does not start; the CI `sanitizers` job
  installs `dbus`. The bus socket goes to `/tmp` when `$TMPDIR` is too long
  for a Unix socket path. `Platform: portal trigger descriptions compare by
  modifiers and key, and shortcut descriptions are made valid UTF-8` needs
  neither.

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
- RealtimeKit over D-Bus (`org.freedesktop.RealtimeKit1`, the `rtkit`
  package; most desktops run it for PipeWire). When the callback's own
  attempt leaves the thread at `SCHED_OTHER`, the engine host asks rtkit
  from the message thread (docs/11 E44): the first callback on each new
  device thread records its kernel thread id (no allocation: the saved
  policy is thread-local), and the host's 5 Hz timer reads that thread's
  policy (`RealtimeScheduling::queryThread`) and calls
  `RealtimeScheduling::requestRealtimeKit` once. That opens a private
  connection to the system bus (`$DBUS_SYSTEM_BUS_ADDRESS`, else
  `/var/run/dbus/system_bus_socket`) through the libdbus-1 the Wayland
  hotkeys load at run time, reads rtkit's `MaxRealtimePriority` and
  `RTTimeUSecMax` (rtkit older than 0.11 has neither: its defaults 20 and
  200 ms are used), lowers the process's `RLIMIT_RTTIME` soft and hard
  limits to `RTTimeUSecMax` (rtkit refuses a process above it), and asks
  `MakeThreadRealtime (tid, min (20, MaxRealtimePriority))`; rtkit sets
  `SCHED_RR | SCHED_RESET_ON_FORK`. From then on a real-time thread of the
  process that runs `RTTimeUSecMax` without blocking gets `SIGXCPU` /
  `SIGKILL`, rtkit's watchdog; the audio thread blocks every period. Each
  call waits at most 1 s. rtkit is asked only for a device the host opened
  itself, never for the message thread.

The outcome is `EngineStatus::audioThread` (`EngineController::getStatus()`;
no panel shows it yet): "real-time (RR 20 via rtkit)", "real-time (FIFO 83,
the audio server's thread)", or "NOT real-time: <reason>; <fix>" (install
rtkit, an `rtprio` limit, or the JACK device type). Not yet: the Realtime
portal (`org.freedesktop.portal.Realtime`) for Flatpak, and runs on real
desktops (tested against a mock rtkit only).

Tests: `Platform: promoteAudioThread allocates nothing …` (the saved
policy, and the thread's policy read back by its id) and `Platform:
RealtimeKit is asked for the audio thread …` in `tests/test_platform_linux.cpp`
run against a mock rtkit on a private `dbus-daemon` that checks what rtkit
checks (the caller's pid from the bus, the thread among its tasks, the
priority, the `RLIMIT_RTTIME` hard limit in `/proc/<pid>/limits` at the
call): the call sequence, the cap at a lower maximum, old rtkit, a refusal,
no rtkit and no bus. The requests run in a forked child, so the test binary
keeps its limits. `tests/app/test_app_host_realtime.cpp` covers the host's
side with a stand-in scheduler.

When the engine runs as a JACK or PipeWire client (JUCE's JACK backend on
`pipewire-jack`), the server calls the process callback on its own RT thread,
which is the recommended setup.

`SystemTuning::disablePowerThrottling()` is a no-op on Linux. CPU frequency
policy is system-wide (cpufreq governor, power-profiles-daemon).

## Latency

PipeWire processes the whole graph in cycles of one *quantum*: the smallest
`node.latency` any running node asks for, within the server's
`clock.min-quantum` / `clock.max-quantum`. The null sink and its monitor add
up to one quantum. A client that asks for nothing gets its library's
default, which is 1024/48000 = 21.3 ms for pipewire-jack.

The app therefore asks for less (docs/11 E48a). When it creates its
router, on the message thread before the audio device opens, it exports
`PIPEWIRE_LATENCY=256/48000` (5.3 ms, the Balanced request). pipewire-jack
and PipeWire's ALSA plug-in read the variable when Flubsound opens its
streams. This is a request, not a lock: the quantum is not locked, and
other clients can still ask for less (the graph follows the smallest
request). While Flubsound runs, the graph quantum drops to 256 unless
another client asks for less or the server's `clock.min-quantum` is higher.
A `PIPEWIRE_LATENCY` you exported yourself is kept (a value that is not
`<frames>/<rate>` gets a note on stderr). Pure JACK2 servers, ALSA `hw:`
devices and PulseAudio ignore the variable.

The Low Latency request (`128/48000`, with `node.lock-quantum = true`
through `PIPEWIRE_PROPS`) is implemented and tested in
`pipewire::planLatencyEnvironment`. It is not used yet: PipeWire reads the
variables only when a stream opens, so a profile change would have to
export them and re-open the device, and the latency profile is not known
when the router is created. To force a smaller quantum for everything, run

```sh
pw-metadata -n settings 0 clock.force-quantum 128
```

## Linking the sinks to the engine

The engine reads each strip from its device input, starting at the strip's
channel (*Settings › Processing › Input feeds strip*, or several strips
through the `deviceInputMap` setting, for example `Game=0;Music=8`).
PipeWire does not connect a sink's monitor to that input by itself. The app
does it, so no qpwgraph or Helvum step is needed.
`LinuxAppAudioRouter::connectEndpointInputs`, called on every pass of the
routing worker (`AppRouting`, every 2 s), works as follows:

- It runs `pw-dump` and reads its JSON with `flub::json`
  (`pipewire::parseDump`).
- Flubsound's own input node is the one belonging to this process with the
  most audio input ports: its `application.process.id`, or its client's
  `pipewire.sec.pid`, is this process. Under pipewire-jack that is the
  JACK client with ports `in_1` … `in_N`.
- Monitor port *k* of each mapped `flubsound_<strip>` sink (the strip's
  endpoint id) is linked to input port *first + k*
  (`pipewire::planMonitorLinks`). A sink gets as many links as it has
  monitor ports (8 for Game, 2 for the others), but never reaches the next
  mapped strip's first channel or runs past the last input.
- Missing links are made with `pw-link <out> <in>`. The command line
  carries port ids (integers) only, so no sink or port name ever reaches
  the shell. "File exists" (another linker was faster) counts as done.
- Links made by others (JUCE's own JACK connections, a microphone) are left
  alone. Links the router made and no longer wants, because the map changed
  or device input was switched off, are removed with `pw-link -d`.
- `pw-dump` costs a few milliseconds of CPU on a busy graph. An unchanged map
  is therefore checked again only every 10 s (links vanish when the device
  re-opens). A pass that changed something is confirmed on the next one.
- What cannot be linked goes to stderr (`Flubsound: PipeWire links: …`),
  once each time the message changes. Examples: a missing sink ("create
  the Flubsound sinks with … install"), a strip mapped past the input's
  channel count, a sink cut short by the next strip, or an input that is
  not a PipeWire node (an ALSA `hw:` device: choose the JACK device type).
  The same text is returned as the call's status for a later UI.
- Without `pw-dump` / `pw-link` (package `pipewire-bin` on Debian / Ubuntu,
  `pipewire-utils` on Fedora, `pipewire` on Arch; Flatpak) the router says
  so and names qpwgraph or Helvum as the manual route. When `pw-dump` fails
  (no PipeWire, for example classic PulseAudio), it suggests choosing a
  sink's monitor as the input device.

The links keep the sink's own port order, as JUCE's own JACK connections
do. The engine reads which speaker each of its inputs carries from the
input's channel names (the monitor ports `monitor_FL` … `monitor_SR` when
the chosen JACK input device is a Flubsound sink) and puts a 2.0 / 5.1 /
7.1 strip into its own order, so a sink made with another channel map still
reaches the right virtual speakers (docs/11 E27). A JUCE ALSA device on a
sound card (`hw:`) is read from the card's capture channel map instead
(`AudioChannelMaps::queryInputPositions`: `snd_pcm_query_chmaps_from_hw`,
libasound loaded at run time, the device found by the name JUCE lists it
under). ALSA plug-in devices (`default`, `pipewire`, `pulse`) report no
map; they deliver ALSA's own 5.1 / 7.1 order, FL FR RL RR FC LFE SL SR,
which the engine permutes back.

JUCE's JACK device gives the engine as many inputs as the chosen *input
device* (a JACK client) has output ports. Choosing "Flubsound Game" gives 8,
enough for the Game strip only. Feeding four strips from one device needs an
input client with 14 ports, which is what the native filter node below
removes.

Tests in `tests/test_platform_linux.cpp` need no PipeWire. `Platform:
PipeWire quantum request …` covers the environment plan (Balanced,
Low Latency, a user's value kept) and the router's request at creation.
`Platform: pw-dump JSON is read …` and `Platform: PipeWire monitor links
follow the device input map …` cover parsing and planning against a
trimmed `pw-dump` fixture: port order, a pid only on the client object,
MIDI ports, another process's JACK client, limits and the problem texts.
`Platform: the Linux router links the sink monitors with pw-link …` runs the
router against fake `pw-dump` / `pw-link` scripts on `PATH`: nine links
made, a confirming pass with no changes, no `pw-dump` before the re-check
interval, the unlinking on a map change and on device input off, and the
message when the tools are missing. `Platform: ALSA channel maps …` covers
the card PCM names, JUCE's device names and the chmap positions (no sound
card is needed; a real card's map has not been read yet), and
`tests/app/test_app_host_io.cpp` the round-trip channel check with fake
JACK and ALSA devices whose channel names or maps come in several orders.

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
- The Low Latency quantum request (`128/48000`, locked) on a profile change,
  which needs the device to re-open after the variables change. Links through
  libpipewire registry events instead of polling `pw-dump`.
- The output's `device.bus` for headset connection detection, and the
  Realtime portal for the audio thread under Flatpak.
