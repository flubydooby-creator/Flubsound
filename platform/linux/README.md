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
  whether the stream is playing (`corked == false`). A stream of a native
  PipeWire client (`pw-play`, SDL or OpenAL on their PipeWire back-end)
  carries none of these itself; they come from its client (the stream's
  `client` in `pactl --format=json list clients`, asked only when such a
  stream exists). Flubsound's own streams and streams without a pid are
  skipped.
- **`setAppEndpoint(pid, sink)`** runs `pactl move-sink-input <id> '<sink>'`
  for every stream of that pid. An empty sink means `@DEFAULT_SINK@`. The
  sink argument passes a strict whitelist (`[A-Za-z0-9_.:@+-]`, no leading
  `-`, max 255 characters) and is single-quoted as well. Nothing unsanitised
  ever reaches `popen`. WirePlumber remembers a stream's move target as the
  application's restore-stream entry and sends its next streams there, so
  a move back to the default also deletes the stream's `target.object` /
  `target.node` in the "default" metadata (`pw-metadata -d <node id> …`,
  docs/11 E47): pipewire-pulse 1.0 already writes -1 (no target) for a move
  to `@DEFAULT_SINK@`, older versions the default sink's own id, which
  would pin the application to that device. The route journal
  (`route-journal.json`, next to the settings) makes the next start undo
  the moves of a run that was killed.
- **`connectEndpointInputs()`** links each strip sink's monitor to the
  engine's input through the libpipewire registry, or with `pw-dump` /
  `pw-link` in a build without libpipewire (see *Linking the sinks to the
  engine* below).
- **`openSystemRoutingSettings()`** starts `pavucontrol --tab=1` (Playback),
  or failing that `pwvucontrol`, GNOME Settings › Sound, or KDE's audio KCM.

`pactl --format=json` needs `pactl` 16 or newer. That covers
pulseaudio-utils 16+ and every current distribution; pipewire-pulse only
provides the server side. The calls block for roughly 5–30 ms, so the app
runs them on its routing worker thread (`AppRouting`, every 2 s while routes
exist), never on the message or audio thread.

**Flatpak.** The sandbox has no `pactl`, `pw-dump` or `pw-link`. Linking
and the native node (below) use libpipewire and need only access to the
PipeWire socket; moving applications still runs `pactl`, so the Flatpak
build has to bundle it (with `--socket=pulseaudio`) or move streams through
libpipewire's `target.object` metadata instead (not done yet).

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
through `PIPEWIRE_PROPS`) is exported by `EngineController` when the user
chooses Low Latency; PipeWire reads the variables only when a stream opens,
so a JACK or ALSA device is re-opened then (a brief dropout). The native
node (below) sets the same values as its own `node.latency` /
`node.lock-quantum` and changes them in place, without a re-open. To force
a smaller quantum for everything, run

```sh
pw-metadata -n settings 0 clock.force-quantum 128
```

## Linking the sinks to the engine

The engine reads each strip from its device input, starting at the strip's
channel (*Settings › Routing › Input feeds strip*, or several strips
through the *Input map* on the same page, stored as the `deviceInput.map`
setting, for example `Game=0;Music=8`; **Fill in one after another** sets
Game 1–8, Music 9–10, Chat 11–12 and System 13–14).
PipeWire does not connect a sink's monitor to that input by itself. The app
does it, so no qpwgraph or Helvum step is needed.
`LinuxAppAudioRouter::connectEndpointInputs`, called on every pass of the
routing worker (`AppRouting`, every 2 s), works as follows:

- In a build with libpipewire (docs/11 E48; `pkg-config libpipewire-0.3`
  found at configure time) it keeps its own PipeWire connection
  (`pipewire::RegistryLinker` in `app/Source/platform/pipewire/`). Registry
  events keep a mirror of the nodes, ports, links and clients current
  (`pipewire::addGlobal` / `removeGlobal`), and every change is re-planned
  20 ms after the last event of a burst, so a re-opened device's new ports
  are linked at once instead of at the next 10 s check. Links are made
  through the link factory with `object.linger = false`: they belong to
  Flubsound's connection and go away with it, also after a crash. The plan,
  the problem texts and the status are the same as below. When no server
  answers, the router falls back to `pw-dump` / `pw-link` and tries the
  connection again every 10 s.
- Otherwise it runs `pw-dump` and reads its JSON with `flub::json`
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
input client with 14 ports, which is what the native node below provides.

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

## Native PipeWire node (docs/11 E48)

In a build with libpipewire, `app/Source/platform/pipewire/` hosts the
engine in Flubsound's own PipeWire node, with no JACK client and no manual
links:

- **The node.** `NativeAudioNode` (`PlatformServices.h`, implemented in
  `PipeWireNative.cpp`) is one `pw_filter` with
  `PW_FILTER_FLAG_RT_PROCESS`: an input port group per strip, `game_FL` …
  `game_SR` (7.1), `music_FL` / `music_FR`, `chat_*`, `system_*` (14
  ports, in the engine's default input map 0 / 8 / 10 / 12), and an output
  group `out_FL` / `out_FR`. Its process callback runs on PipeWire's
  real-time data thread (`client-rt.conf` on PipeWire 1.0, whose
  `module-rt` uses `SCHED_FIFO` or RealtimeKit). It fetches each port's
  buffer and hands them to `pipewire::CycleRunner`, which splits a quantum
  larger than the prepared block, reads silence for a port without a
  buffer and never allocates or locks.
- **Sinks from the app.** A `flubsound_<strip>` sink that does not exist is
  created by the node (`support.null-audio-sink` through the adapter
  factory, with the names, descriptions and channel positions of
  `90-flubsound-sinks.conf`), so no setup script is needed. They are not
  lingering: they vanish with Flubsound's connection, also after a crash,
  and the desktop moves their streams back to the default output. Sinks
  that exist already (the script or the config file) are used as they are.
- **Links.** The node links each sink's monitor ports to its strip's ports
  by channel (`pipewire::planStripLinks`: `monitor_FC` to `game_FC` even if
  the sink lists its channels in another order; a mono sink feeds both
  ports of a stereo strip) and its outputs to the default output, the
  `default.audio.sink` of the "default" metadata, or to a sink the app names
  (`pipewire::planOutputLinks`, `setOutputTarget`). It never plays into one
  of its own sinks (a feedback loop): when the default output is one of
  them (System made the default so that everything else is processed), it
  plays to the real sink with the highest `priority.session` instead and
  says so in its status (`pipewire::chooseOutputSink`). Registry events re-plan every change,
  as for the JUCE path above.
- **Latency.** `node.latency` is the profile's request, 256/48000 on Quality
  and Balanced and 128/48000 on Low Latency, and `setLatency` changes it in
  place (`pw_filter_update_properties`) without a re-open. A lock sent with
  the new latency would freeze the old quantum, so Low Latency's
  `node.lock-quantum` follows once the graph runs at 128 (checked every
  20 ms for up to 1 s; a client that holds a larger quantum leaves the
  request unlocked, with a note on stderr). The graph's actual quantum and
  rate at the last cycle are in `NativeAudioNodeStatus`.
- **Status.** `NativeAudioNode::getStatus()` counts the links wanted and
  made each way, names the output sink and the sinks the node created, and
  says what is missing (`pipewire::describeLinks` makes it one line for the
  routing panel).
- **As a JUCE device.** `pipewire/PipeWireDeviceType.*` wraps the node as
  a `juce::AudioIODeviceType` named "PipeWire" with one device, "Flubsound
  Engine" (its input side "Flubsound Engine inputs", so the host's feedback
  guard does not take one virtual device on both ends for a loop). Its 14
  input channels are named `Game:FL` … `System:FR`, which
  `AudioEngineHost::positionFromChannelName` reads. The callback swap is
  lock-free. `pipewire::addDeviceType` adds the type after JUCE's own;
  `setDeviceLatency`, `setDeviceOutputTarget` and `getDeviceStatus` reach
  the running device. All 14 inputs are always active (they are the
  strips' sinks, not a hardware selection), and the device reports no
  latency of its own: the node runs in the same graph cycle as the sinks it
  reads, and the graph's quantum is reported separately.
- **In the app.** Settings › Audio lists the "PipeWire" device type
  (`AudioEngineHost::openDevice` adds it). On a first start (nothing saved)
  the app opens it by itself when a PipeWire server answers, so a fresh
  install needs no setup script and no manual links; JUCE's own type
  otherwise, and nothing is saved until the user picks a device. Each
  strip reads its own sink (the channel names give the map: Game 0-7,
  Music 8-9, Chat 10-11, System 12-13; a `Game=0;Music=8` map typed in the
  settings still wins). A latency profile change sets the node's
  `node.latency` in place, with no re-open and no dropout. The quantum the
  graph runs at is part of the header's latency total ("+ audio graph").
  The routing panel shows the node's line above its buttons, e.g.
  *PipeWire: Linked 14 of 14 input channels, output to
  alsa_output.usb-… (2 of 2), quantum 256/48000*, in amber when a link is
  missing; with a JUCE device it shows what the registry links could not
  make, if anything.
- **libpipewire at run time (R1.2).** The app is built against
  libpipewire-0.3's headers but does not link the library:
  `PipeWireLibrary.cpp` opens `libpipewire-0.3.so.0` with `dlopen` the
  first time a PipeWire connection is wanted and resolves 30 exported
  functions: the 29 `PipeWireNative.cpp` calls and
  `pw_get_library_version` (`PipeWireApi.h`; what the headers implement
  inline needs no symbol). A missing one leaves the library unloaded, with
  its name in the reason. One binary therefore starts on every
  distribution. Without the library, `pipewire::library()` says why
  (*PipeWire's client library (libpipewire-0.3.so.0) is not installed*),
  the "PipeWire" device type is not offered, `NativeAudioNode` reports
  itself unsupported with that reason and the routing falls back to
  `pw-dump` / `pw-link` (which also fail without PipeWire, with their own
  message). stderr names the run-time and build-time versions once
  (*libpipewire 1.0.5 opened at run time (built against 1.0.5)*).
- **PulseAudio without PipeWire.** There is no native PulseAudio device
  type: the app plays through JUCE's ALSA type, whose `default` (or
  `pulse`) PCM is PulseAudio's ALSA plug-in (`pulse-alsa`, package
  `libasound2-plugins` / `alsa-plugins-pulseaudio`); `pactl` per-app
  routing and the `flubsound_*` null sinks work the same on PulseAudio
  (`flubsound-pipewire-setup.sh print-pa-config`). The monitor links are
  PipeWire-only: on PulseAudio the router suggests choosing a sink's
  monitor as the input device (above). This path is documented, not
  tested: no CI job or run here has had a PulseAudio-only system.
- **Which type a first start opens.** With nothing saved the app prefers
  the "PipeWire" type only when a PipeWire server answers *and* plays the
  audio: its graph has an output sink that is not one of Flubsound's own
  (`pipewire::serverPlaysAudio`, which connects a probe session for a few
  ms, and the pure check `pipewire::playsAudio`). Otherwise it keeps
  JUCE's default (ALSA) and says why on stderr. The library alone is not
  enough (R1.2 review): many PulseAudio desktops have libpipewire
  installed, and some run a PipeWire daemon for screen capture beside
  PulseAudio, which answers but has no audio sink; the node would play
  into nothing there. In both cases the "PipeWire" entry stays listed.
  The choice is unit-tested with made-up graphs
  (`tests/test_pipewire_cycle.cpp`, every OS) and the probe on CI's
  headless server; no PulseAudio desktop has run it.
- **Xruns.** libpipewire has no xrun event for a client, so the node counts
  its own (`pipewire::XrunCounter`, after each cycle on the data thread):
  a cycle it finished after the next one was due (the driver's `nsec` +
  one quantum, PipeWire's own xrun), or a position that skipped ahead on
  the same driver (cycles run without it); a new driver, rate or a restart
  re-bases without counting. The first two cycles of a run, and the first
  two after the filter enters `STREAMING` again (a pause, during which the
  graph may run on without it), are not judged
  (`XrunCounter::kSettleCycles`, `restart()` from the state callback;
  R1.2 review: a run's first cycle was often late on an idle server, which
  showed `· 1 xr` after an open). The device reports it through
  `getXRunCount()`, so the header shows `· 3 xr` and the overload watchdog
  counts the node's xruns as it counts a JUCE backend's. Each callback
  also carries the driver's time (`hostTimeNs`: the cycle's start plus the
  block's offset), so the callback timing (docs/11 E45) follows the
  graph's cadence even when a large quantum is split into blocks. Its
  intervals are therefore the driver's clock, not when the data thread
  woke: a late wake-up shows as an xrun, not as a late interval.
- **When PipeWire takes the node away.** If the server quits or restarts
  (the connection drops) or removes the node (`pw-cli destroy`, a
  patchbay), the node tells the device once (`nodeError`, on the loop
  thread), the device hands it to the message thread and the host shows
  the device error and runs its recovery (docs/11 E51): after the settle
  time it re-opens the device, which connects again and makes a new node
  and new sinks; with the server still away it retries with back-off and
  then keeps the error banner. The device reports to the host directly
  (`pipewire::setDeviceErrorTarget`, registered in
  `AudioEngineHost::audioDeviceAboutToStart`): JUCE 9.0.2's
  `AudioDeviceManager` starts devices through a wrapper that drops
  `audioDeviceError`. Tested on CI with `pw-cli destroy` (the error 12 – 14
  ms after it, callbacks again 260 – 281 ms after it with the test's 100 ms
  settle time; the app waits 1.5 s); a server restart takes the same path
  but is not tested. Closing the device drops an error the message thread
  has not handled yet (R1.2 review): JUCE re-opens the same device object
  for a new rate or buffer size, and the old node's error must not reach
  the new run as a device-error banner.

Also open: the Flatpak build with the Realtime and GlobalShortcuts
portals, a headless mode for SteamOS Game Mode, moving applications
through `target.object` metadata, and the manual matrix of the item
(Fedora, Ubuntu, KDE Neon, SteamOS) on real sound cards.

Tests: `tests/test_platform_linux.cpp` covers the registry mirror, the
plans, the port names, the latency values and the cycle runner with fakes
on every Linux build. `tests/app/test_app_pipewire.cpp` runs the node, the
registry linker and the device type (with a real `AudioEngineHost`)
against a running PipeWire server: sinks made and removed, 10 of 10 strip
links and 2 of 2 output links, a tone through a sink to the right strip
channels only, the quantum at 256 and then 128 without a re-open, no
allocation and no lock on the data thread over 100 cycles, the feedback
refusal, and the device's sinks gone after it closes. The app-level case
runs an `EngineController` with a device on a fresh settings file: the
"PipeWire" type is offered and taken on the first start, all four strips
read their sinks (tones into `flubsound_game` and `flubsound_chat` reach
those strips, processed audio reaches the output), Low Latency moves the
quantum to 128 with the same device still running, the latency total is
engine + graph quantum, and the routing panel's line reads *Linked 14 of
14 input channels*. `tests/app/test_app_route_journal_linux.cpp` is the
route journal's crash test: a child `flub_app_tests` moves a `pw-play`
stream to a Game sink through the real `pactl` router and is killed with
SIGKILL; WirePlumber then sends the application's next stream to the
Game sink too; the restart moves both back to the default output, and
with another default output the application's next stream follows it (the
restore-stream entry is gone). R1.2 added four cases there: the run-time
loading (a missing soname is reported and offers no device type; the
installed library resolves every entry point; this one needs no server),
the device's xruns and time stamps (0.6 s on the idle test server: one
callback per block at the graph's cadence, none late, no xrun; since the
review also on the wall clock, read by a second callback next to the
driver's stamp), the hot-plug (a second sink made the default takes the
outputs, and they move to the remaining sink when it goes, without a
re-open) and the recovery (`pw-cli destroy` on the device's node: the host
gets the error and re-opens the device with a new node by itself). The
review added a fifth, the stale error (a node error still pending when
JUCE re-opens the same device object for a new buffer size never reaches
the host). `tests/test_pipewire_cycle.cpp` tests the xrun count (with the
settling after a start or a pause), the block time stamps and the first
start's choice with made-up clocks and graphs on every OS. CI's `app` job also hides
`libpipewire-0.3.so.0` for one step: the app still renders and the loading
case takes its "not installed" branch. Without a server the tests print
"skipped"; the app-level and crash cases also skip on a server with real
(ALSA or Bluetooth) devices, since they open the default output or change
it. CI's `pipewire` job runs them 20 times in a row against a headless
server. To run them headless here (the runtime folder's path must be
short: the socket path has to fit in 108 bytes):

```sh
export XDG_RUNTIME_DIR=$(mktemp -d /tmp/pw.XXXXXX)   # a private runtime folder
export XDG_STATE_HOME=$XDG_RUNTIME_DIR/state XDG_CONFIG_HOME=$XDG_RUNTIME_DIR/config
dbus-daemon --session --fork --address=unix:path=$XDG_RUNTIME_DIR/bus
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
pipewire & sleep 1; wireplumber & pipewire-pulse &   # pipewire, wireplumber, pipewire-pulse, pulseaudio-utils
flub_app_tests "(E48"; flub_app_tests "E47 Linux"
```

## Headset profiles on Linux

`AudioEndpoints::queryOutputTransport()` returns *unknown* on Linux: JUCE's
ALSA / JACK device names do not map one-to-one to PipeWire / Pulse sinks, so
the PipeWire `device.bus` property is not queried. Headset profiles therefore
detect Bluetooth and hands-free outputs from the device name and format
(`flub::device::detectConnection`, see `docs/10-headset-compatibility.md`).

## Roadmap

- **Native PipeWire node:** built and offered in the app (above, the
  first-run default when a PipeWire server answers); Flatpak and Game
  Mode's headless mode are open, and the CI job stays non-blocking until
  20 consecutive runs are green.
- libpipewire routing (`target.object` metadata) instead of shelling out to
  `pactl`, for Flatpak and to receive change events instead of polling.
- Wayland hotkeys: run on real desktops (so far tested only against a mock
  portal), pass a `parent_window` and ship a `.desktop` file so the portal
  knows the application id.
- The output's `device.bus` for headset connection detection, and the
  Realtime portal for the audio thread under Flatpak.
