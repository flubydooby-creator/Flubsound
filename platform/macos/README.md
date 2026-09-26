# Flubsound on macOS: virtual device, per-app capture, distribution

Status: **design.** `app/Source/platform/PlatformServices_mac.mm` implements
global hotkeys (Carbon `RegisterEventHotKey`) and thread tuning
(`THREAD_TIME_CONSTRAINT_POLICY`). Per-app routing and capture report
`isSupported() == false` until the design below is implemented and tested on
real hardware.

macOS offers two complementary mechanisms, and Flubsound uses both:

| Mechanism | macOS | What it gives us |
|---|---|---|
| **Core Audio process taps** | 14.2+ | Capture any app's audio and **mute its original output** while tapped, so routing works without a driver. This is the primary per-app path. |
| **Audio Server Plug-in** "Flubsound Virtual Device" | 11+ | Output devices "Flubsound Game / Music / Chat / System" and an input device "Flubsound Mic". Used for apps with their own device menu, for older macOS, and for the processed microphone. |

---

## 1. Per-app capture and routing with process taps (macOS 14.2+)

1. **List audio clients.** Read `kAudioHardwarePropertyProcessObjectList`.
   For each process object read `kAudioProcessPropertyPID`,
   `kAudioProcessPropertyBundleID` and
   `kAudioProcessPropertyIsRunningOutput`. These fill `AudioSessionInfo`.
2. **Tap a process.**
   - Translate the pid with `kAudioHardwarePropertyTranslatePIDToProcessObject`.
   - Create the tap description:
     `[[CATapDescription alloc] initStereoMixdownOfProcesses:@[obj]]`, or
     `initWithProcesses:andDeviceUID:withStream:` for multichannel.
   - Set `privateTap = YES` and `muteBehavior = CATapMutedWhenTapped`. The app
     keeps playing to its device as far as it knows, but only Flubsound's
     processed copy is audible.
   - Call `AudioHardwareCreateProcessTap`.
   - "Everything except Flubsound" (the equivalent of Windows' `EXCLUDE`
     loopback mode) is `exclusive = YES` with our own process object in the
     list.
3. **Private aggregate device containing only the tap.** Create it with
   `AudioHardwareCreateAggregateDevice` and these keys:
   - `kAudioAggregateDeviceIsPrivateKey = 1`
   - `kAudioAggregateDeviceTapListKey = [{ kAudioSubTapUIDKey, kAudioSubTapDriftCompensationKey = YES }]`
   - `kAudioAggregateDeviceTapAutoStartKey = 1`
   - main sub-device = the current output device, which acts as the clock.

   Read the format from `kAudioTapPropertyFormat`. Then call
   `AudioDeviceCreateIOProcIDWithBlock` and `AudioDeviceStart`. The IO block
   pushes frames into the strip's `SpscRing`.
4. **Restore.** Stop the device, destroy the IOProc, the aggregate device and
   the tap. The process is unmuted automatically.

Requirements and caveats:

- `NSAudioCaptureUsageDescription` in `Info.plist`. The user grants
  "System Audio Recording" in *Privacy & Security* on first use.
- Core Audio has no process-tree flag. Child and helper processes (for
  example browser renderers, or game launchers that spawn the game) must be
  added explicitly. Re-scan the process list when
  `kAudioHardwarePropertyProcessObjectList` changes.
- The API is young. Validate tap creation and teardown on every macOS minor
  release, and gate the feature with `@available(macOS 14.2, *)`.

## 2. Audio Server Plug-in: "Flubsound Virtual Device"

### 2.1 Code base and licence

- Build it on **[libASPL](https://github.com/gavv/libASPL)** (MIT). It is a
  C++17 library that implements the `AudioServerPlugInDriverInterface`
  boilerplate: the object model (Plugin, Device, Stream, volume and mute
  controls), property dispatch, and `ControlRequestHandler` /
  `IORequestHandler` hooks.
- Apple's "Creating an Audio Server Driver Plug-in" sample (NullAudio,
  Apple sample-code licence) is a behavioural reference.
- **Do not use BlackHole.** It is GPL-3.0, and deriving from it would force
  the plug-in (and arguably the bundle) under the GPL. Follow a clean-room
  rule: engineers working on the plug-in do not read BlackHole sources.

### 2.2 Devices

| Device | Direction | Channels | Notes |
|---|---|---|---|
| Flubsound Game | output | 8 (7.1) | folded to binaural by the Game strip |
| Flubsound Music | output | 2 | |
| Flubsound Chat | output | 2 | |
| Flubsound System | output | 2 | suggested system output |
| Flubsound Mic | input | 2 | processed microphone for chat apps |

Formats: float32 at 44.1, 48 and 96 kHz. Nominal rate changes are applied
through libASPL's control requests (`RequestConfigurationChange`).

### 2.3 Data path

- **v1: loopback devices.** Each output device also has an input stream. In
  the plug-in, `WriteMix` writes into a lock-free ring and `ReadInput` reads
  from the same ring. The engine opens the devices' input side like any
  input device. There is no IPC, but each device is an extra HAL client and
  its clock differs from the real output (see 2.4).
- **v2: shared memory (zero copy).** Plug-ins run in the sandboxed
  `Core-Audio-Driver-Service` helper. They may look up Mach services listed
  under `AudioServerPlugIn_MachServices` in the plug-in's `Info.plist`.
  1. The Flubsound app, or a tiny LaunchAgent, vends an XPC service.
  2. The plug-in connects from a **non-real-time** thread (`StartIO`) and
     receives a shared-memory region (`xpc_shmem_create`).
  3. That region uses the same portable ring layout as Windows: the
     `FLUB_VA_STREAM_CONTROL` part of
     `platform/windows/driver/FlubVirtualAudioShared.h`.
  4. `WriteMix` / `ReadInput` touch only that ring. With no engine attached
     the plug-in discards output and produces silence, so the devices stay
     usable.

### 2.4 Clocking

- The default is a host-time clock (`mach_absolute_time`, nominal rate)
  reported through `GetZeroTimeStamp`. It drifts against the real output
  device by ±20–100 ppm, so the engine runs its ASRC. An alternative is a
  private aggregate device of the real output plus the virtual device with
  `kAudioSubDeviceDriftCompensationKey`, which lets the HAL do the drift
  correction.
- **v2 slaving.** The engine publishes the real device's IOProc timestamps
  (`mSampleTime`/`mHostTime` pairs, `mRateScalar`) into shared memory, and
  `GetZeroTimeStamp` derives the virtual zero timestamps from them. The
  virtual device then inherits the real clock and no ASRC is needed. If the
  engine stops, the plug-in falls back to the host clock.

### 2.5 Real-time rules inside the plug-in

libASPL's IO handlers run on coreaudiod's real-time IO thread. They must not
allocate, lock, send Objective-C messages, make XPC calls or log. They only
touch the pre-mapped ring.

A plug-in bug takes down **all** audio on the machine, so keep the plug-in
tiny: no DSP.

## 3. Engine threads on Apple Silicon

`SystemTuning::promoteAudioThread` applies the Mach time-constraint policy.
Threads that work in lock-step with a device should also **join the
device's audio workgroup** (`kAudioDevicePropertyIOThreadOSWorkgroup`, exposed
by JUCE as `AudioWorkgroup`). This keeps them on performance cores next to
the IO thread.

`disablePowerThrottling` is a no-op on macOS. App Nap does not throttle a
process while Core Audio IO is running.

## 4. Packaging, signing and notarisation

1. Build **universal binaries** (arm64 + x86_64) for the app and
   `FlubsoundVirtualDevice.driver`. coreaudiod is arm64 on Apple Silicon.
2. Sign inside-out without `--deep`, using a **Developer ID Application**
   certificate:
   `codesign --force --options runtime --timestamp --sign "Developer ID Application: …" <bundle>`.
3. App entitlements and Info.plist:
   - `com.apple.security.device.audio-input` (the microphone under the
     hardened runtime);
   - `NSMicrophoneUsageDescription` and `NSAudioCaptureUsageDescription`.
4. Build the installer with `pkgbuild` / `productbuild`. The plug-in installs
   to `/Library/Audio/Plug-Ins/HAL/`, which needs admin rights; the App Store
   is therefore not an option for the plug-in. Sign the package with a
   **Developer ID Installer** certificate. The postinstall script runs
   `launchctl kickstart -k system/com.apple.audio.coreaudiod` to reload
   coreaudiod.
5. **Notarise:**
   `xcrun notarytool submit Flubsound.pkg --keychain-profile flub --wait`,
   then `xcrun stapler staple Flubsound.pkg` (and the `.dmg` / `.app`).
6. Uninstaller: remove the bundle, restart coreaudiod, and remove the
   LaunchAgent if one is installed.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Plug-in crash or hang kills system audio | Minimal plug-in, no DSP, fuzzed property handlers, RT-safety review |
| Tap API bugs or behaviour changes across macOS updates | Feature gate per OS version, beta-seed CI machines, fall back to device selection |
| TCC prompts confuse users | Explain before the first tap; deep-link to Privacy & Security |
| Driver-service sandbox changes (Mach lookups) | Keep the v1 loopback path as a fallback |
| Clock drift between virtual and real devices | Engine ASRC; v2 timestamp slaving |
