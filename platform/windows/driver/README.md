# Flubsound Virtual Audio — Windows WaveRT driver (design)

Status: **design, not implemented.** The C header
[`FlubVirtualAudioShared.h`](FlubVirtualAudioShared.h) is the only code here.
It fixes the user/kernel contract (control block layout, IOCTL codes, endpoint
ids) so the engine side can be built and unit-tested before the driver exists.

---

## 1. Why Flubsound needs its own driver

Per-app processing ("game through the Game strip, Spotify through the Music
strip") needs **audio endpoints that applications can render to**. Windows
has no built-in virtual devices, and each alternative falls short:

| Option | Why it is not enough on its own |
|---|---|
| Process loopback capture (`ProcessLoopbackCapture`, Win10 20348+/Win11) | Copies an app's audio but cannot silence the original, so the user hears the unprocessed signal too. Not available on Windows 10 22H2. |
| APO on the real device (Equalizer APO model) | Processes the final system mix. It cannot tell apps apart, it is fragile across driver updates, and the Windows 11 APO rules restrict third-party APOs. |
| Third-party cables (VB-Cable, VoiceMeeter) | Cannot be redistributed, have their own licence terms, and are a support burden. |

So Flubsound installs a PortCls **WaveRT** driver, "Flubsound Virtual Audio".
It exposes five endpoints. Applications are routed to them in Windows'
per-app settings or through `AppAudioRouter::setAppEndpoint`. The engine
reads their audio **directly from the drivers' cyclic buffers**, processes
each strip and plays the mix on the real device.

```
 cs2.exe ──► audiodg (mix + SRC) ──► [Flubsound Game  WaveRT buffer] ─┐
 Spotify ──► audiodg ─────────────► [Flubsound Music WaveRT buffer] ─┤ mapped into the engine
 Discord ──► audiodg ─────────────► [Flubsound Chat  WaveRT buffer] ─┤ (IOCTL, zero copy)
 others  ──► audiodg ─────────────► [Flubsound System WaveRT buffer]─┘
                                                                        │
          Flubsound engine: MixEngine strips (EQ, bass, HRTF, limiter) ◄┘
                    │
                    └──► WASAPI (IAudioClient3 low-latency shared / exclusive) ──► headphones

 mic ──► WASAPI capture ──► engine (gate, EQ) ──► [Flubsound Mic buffer] ──► audiodg ──► Discord
```

## 2. Code base and licence

- Start from Microsoft's **SimpleAudioSample**, the reduced SYSVAD in
  `microsoft/Windows-driver-samples/audio/simpleaudiosample`. Use the full
  **SYSVAD** sample as the reference for jacks, packet mode and custom
  property sets. Both are under **MS-PL**. MS-PL allows a closed-source
  binary release, as long as we keep the licence and copyright notices in
  every derived source file. We cannot relicense the derived *source* under
  other terms. Do not mix in GPL virtual-cable code.
- Language: kernel C++ subset (no exceptions, RTTI or STL, no floating point
  in the kernel). Build with the WDK / EWDK, `/W4 /WX /analyze`, Static
  Driver Verifier and CodeQL (both are required for certification). Run
  Driver Verifier (standard + DDI compliance) in every CI test VM.
- No DSP in the kernel. The driver only moves audio and keeps time. All
  processing lives in the user-mode engine (`flub_core`), so a DSP bug can
  never cause a bugcheck.

## 3. Endpoints

Each endpoint is a WaveRT miniport and topology miniport pair (one KS filter
pair, a "subdevice" in SYSVAD terms). Only enabled endpoints are created
(`HKR\Parameters\EnabledEndpoints` bit mask, written by the installer), so
users who only want Music + Chat are not left with five devices.

| Id (`FLUB_VA_ENDPOINT_*`) | Name shown in Windows | Direction | Channels | Node type |
|---|---|---|---|---|
| `GAME` | Flubsound Game | render | 7.1 (`0x63F`) | `KSNODETYPE_SPEAKER` |
| `MUSIC` | Flubsound Music | render | stereo | `KSNODETYPE_SPEAKER` |
| `CHAT` | Flubsound Chat | render | stereo | `KSNODETYPE_SPEAKER` |
| `SYSTEM` | Flubsound System | render | stereo | `KSNODETYPE_SPEAKER` |
| `MIC` | Flubsound Mic | capture | stereo | `KSNODETYPE_MICROPHONE` |

- **Formats.** Each endpoint offers float32, PCM16 and PCM24-in-32 at 44.1,
  48 and 96 kHz, with a fixed channel count per endpoint. The default device
  format is **float32 / 48 kHz**, the engine's native format, so audiodg does
  one float conversion and nothing else. Game is 7.1 so games render real
  surround, which the Game strip's `HeadphoneVirtualizer` folds to binaural.
- **Names.** Each bridge pin gets its own pin-name GUID, registered under
  `HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories\{guid}\Name` by the
  INF (as SYSVAD does). This makes Sound settings show
  "Flubsound Game" rather than "Speakers (Flubsound Virtual Audio)".
- **Jack description.** `KSPROPERTY_JACK_DESCRIPTION(2)` always reports the
  jack as connected, so the endpoints never show as "unplugged".
- **Packet-based WaveRT.** Implement `IMiniportWaveRTStreamNotification`,
  `IMiniportWaveRTOutputStream` (render) and `IMiniportWaveRTInputStream`
  (capture). `SetWritePacket` gives the driver an explicit write position for
  render, which becomes `WritePosition` in the shared control block.
  `KSPROPERTY_AUDIO_PACKETSIZE_CONSTRAINTS2` advertises packets down to 128
  frames. This lets applications use `IAudioClient3` low-latency shared mode
  on our endpoints.
- **Exclusive mode.** Exclusive mode works without extra code. The
  application then writes the same WaveRT buffer that audiodg would have
  written, and the engine cannot tell the difference.

## 4. Zero-copy engine path

### 4.1 What is shared

For every endpoint the driver owns two regions and maps them into the engine
process on request. The layout is fixed in `FlubVirtualAudioShared.h`.

1. **Control page** (4 KiB, `MmAllocatePagesForMdlEx`, zeroed). Only the
   engine gets a writable mapping; audiodg sees the page read-only, and only
   if `ReadOffsetBytes` is exposed as the WaveRT position register. It starts
   with `FLUB_VA_STREAM_CONTROL`: the format description plus the
   `Generation` sequence lock, then 64-bit monotonic frame
   counters `WritePosition` and `ReadPosition`, each on its own 64-byte cache
   line with its QPC timestamp, and finally the state/heartbeat line.
2. **Data region.** This is the WaveRT cyclic buffer itself.
   `AllocateBufferWithNotification` returns an MDL describing a slice of a
   persistent per-endpoint allocation, `DataCapacityBytes` long; 256 KiB is
   enough for 8 ch × float32 × 8192 frames. audiodg writes (render) or reads
   (capture) exactly these physical pages, and the engine reads or writes the
   same pages through its own mapping. **No copy happens in the kernel or in
   audiodg beyond the mix audiodg does anyway.**

Because the data region outlives streams, the engine maps it **once**. When
audiodg re-creates the stream (format change, idle stop/start) the driver
only rewrites the description in the control page, increments `Generation`
and signals the state event. *Prototype spike:* confirm that PortCls accepts
a driver-owned MDL from `AllocateBufferWithNotification` on every supported
build. The fallback is a per-stream buffer with deferred free: the old pages
stay alive until the engine unmaps them, and the engine remaps when
`Generation` changes.

### 4.2 Control interface

- In `DriverEntry`, after `PcInitializeAdapterDriver`, the driver hooks
  `IRP_MJ_CREATE/CLOSE/CLEANUP/DEVICE_CONTROL`. SYSVAD already hooks
  `IRP_MJ_PNP` this way. IRPs for our **control device object** are handled
  by the driver. Everything else is forwarded to `PcDispatchIrp`.
- The control device is a non-PnP control device object created with
  `IoCreateDeviceSecure`, `FILE_DEVICE_SECURE_OPEN`, the class GUID
  `GUID_DEVCLASS_FLUB_VIRTUAL_AUDIO_CONTROL` and the SDDL
  `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)` (SYSTEM, administrators,
  interactive users). It is published with `IoCreateSymbolicLink` as
  `\DosDevices\Global\FlubsoundVirtualAudio` (user mode:
  `\\.\FlubsoundVirtualAudio`). It cannot be a device interface:
  `IoRegisterDeviceInterface` needs a PDO, and putting our SDDL on the
  adapter's devnode would also restrict the KS interfaces audiodg opens. The
  driver deletes the link and the object when the last adapter is removed.
- IOCTLs are defined with `CTL_CODE` (`METHOD_BUFFERED`, vendor device type
  `0x8F1B`):
  - `IOCTL_FLUB_VA_GET_INFO`: magic, ABI version and per-endpoint info. The
    engine refuses to map on an ABI mismatch.
  - `IOCTL_FLUB_VA_MAP_ENDPOINT`: maps the control page and data region into
    the **calling** process with
    `MmMapLockedPagesSpecifyCache (UserMode, MmCached, …, MdlMappingNoExecute)`
    inside `__try/__except`. It references the engine-created, **unnamed**
    `DataEvent` and `StateEvent` handles (`ObReferenceObjectByHandle`,
    `UserMode`, `EVENT_MODIFY_STATE`). Only one mapping per endpoint is
    allowed; a second caller gets `STATUS_SHARING_VIOLATION`. 32-bit (WoW64)
    callers are rejected (`IoIs32bitProcess`): the 64-bit position counters
    are not single-copy atomic in 32-bit code.
  - `IOCTL_FLUB_VA_UNMAP_ENDPOINT`: explicit unmap.
  - `IOCTL_FLUB_VA_SET_CLOCK_MODE`: free-running or engine-slaved (§5).
- Mappings belong to the file object and record the mapping process. A user
  mapping must be removed with `MmUnmapLockedPages` in the mapping process
  and must never outlive it. Normally that happens in `IRP_MJ_CLEANUP`,
  which runs in the context of the process that closes the last handle: the
  engine, including when it is killed (handle rundown). A handle duplicated
  into another process would move the cleanup elsewhere, so the driver also
  registers `PsSetCreateProcessNotifyRoutineEx` and unmaps when the mapping
  process exits. `IRP_MJ_CLEANUP` in a different process only marks the
  endpoint free. Killing the engine therefore never leaks mappings.
- Named events (`Global\Flubsound.VirtualAudio.<n>.State`) exist only for
  diagnostic tools. The driver creates them at load time, before any user
  process can claim the names.

### 4.3 Engine side

1. `CreateFileW (FLUB_VA_CONTROL_PATH_USER)` (`\\.\FlubsoundVirtualAudio`),
   then `GET_INFO`, then `MAP_ENDPOINT` for each enabled endpoint.
2. **Render.** The strip's input is pulled from the output device callback
   (MMCSS "Pro Audio"):
   - `avail = ReadAcquire64 (&WritePosition) - ReadPosition`,
   - convert or copy `min (avail, needed)` frames into the strip block,
   - `WriteRelease64 (&ReadPosition, …)`,
   - update `ReadQpc` and `EngineHeartbeat`.
3. **Mic.** Mirror image: the engine is the producer of `WritePosition`.
4. `Generation` is a sequence lock: the driver makes it odd before it
   rewrites the description and even again afterwards. The engine reads it
   (acquire), retries while it is odd, copies the description, then re-reads
   it and retries on a change, so a format change is never half seen.

## 5. Clocks: slaving versus ASRC

There are two clock domains: the virtual endpoint's "hardware" position,
which audiodg follows, and the real output device's crystal, which drives
the engine.

- **Free-running** (SYSVAD default). A high-resolution kernel timer
  (`ExAllocateTimer` with `EX_TIMER_HIGH_RESOLUTION`) advances the position
  at exactly the nominal rate on the QPC time base. Real DACs are off by
  ±20–100 ppm. At 100 ppm and 48 kHz that is 4.8 frames/s, so a 10 ms
  (480-frame) cushion overflows or underflows in **about 100 s**. The engine
  must then run an **ASRC**: a polyphase windowed-sinc resampler (for
  example 32 taps × 256 phases, more than 110 dB image rejection, about
  0.35 ms group delay) whose ratio a PI controller trims from the ring fill
  level.
- **Engine-slaved** (default whenever the engine is attached and the virtual
  and real sample rates match). The driver advances the render position by
  what the engine has actually consumed (`ReadPosition`/`ReadQpc`), and the
  capture position by what it produced. The virtual device then **inherits
  the real device's clock** and no ASRC is needed: bit-exact audio, zero
  resampler cost, no drift.
  - audiodg expects smooth, monotonic positions with consistent QPC
    timestamps. The driver therefore runs a **delay-locked loop** (as JACK
    does) on the (`ReadPosition`, `ReadQpc`) pairs to estimate rate and
    phase. It reports interpolated positions from `GetPacketCount` /
    `GetOutputStreamPresentationPosition`, and schedules packet events with
    the high-resolution timer at the predicted times. Position never jumps.
  - Watchdog: if `EngineHeartbeat` stalls for more than 2 periods (engine
    crashed or the output device was removed), the driver switches to
    free-running and sets `FLUB_VA_FLAG_ENGINE_TIMEOUT`. Render data is then
    discarded and the mic produces silence, so applications never block or
    glitch-loop. Re-attaching resynchronises the DLL.
- If the rates differ (for example the virtual device runs at 48 kHz and a
  USB DAC runs at 44.1 kHz), the engine resamples anyway. It then uses the
  same ASRC with a fixed nominal ratio plus drift trim, and the driver stays
  slaved.

## 6. Latency budget (48 kHz)

| Stage | Default (10 ms shared periods) | Low latency (128-frame packets, `IAudioClient3`) |
|---|---|---|
| audiodg renders ahead into the virtual buffer | 10.0 ms | 2.67 ms |
| Engine pickup alignment (avg ½ output period) | 5.0 ms | 1.33 ms |
| Engine DSP (limiter look-ahead 1.5 ms + oversampling FIRs ~0.3 ms) | 1.8 ms | 1.8 ms |
| Output WASAPI period on the real device | 10.0 ms | 2.67 ms |
| Device FIFO / codec (typical USB or HDA) | ~2 ms | ~2 ms |
| **Total, application to ear** | **≈ 29 ms** | **≈ 10.5 ms** |

Without the zero-copy path, the engine would record each virtual endpoint
through WASAPI (loopback or a capture pin). That adds one more audiodg pass
and one capture period plus alignment: **+10–15 ms** at the default and
+3–4 ms in low-latency mode. It also costs extra CPU for the additional
mix/SRC pass. The goal for the Game path is **≤ 15 ms** in low-latency mode;
audio lagging the picture becomes noticeable in fast games above about
20 ms.

## 7. INF and installation

INF essentials (a DCH-compliant universal driver):

```ini
[Version]
Signature   = "$WINDOWS NT$"
Class       = MEDIA
ClassGuid   = {4d36e96c-e325-11ce-bfc1-08002be10318}
Provider    = %ManufacturerName%
CatalogFile = FlubVirtualAudio.cat
DriverVer   = 01/01/2027,1.0.0.0
PnpLockdown = 1

[DestinationDirs]
DefaultDestDir = 13                       ; run from the Driver Store (driver package isolation)

[Manufacturer]
%ManufacturerName% = Flubsound, NTamd64.10.0...19041, NTarm64.10.0...19041

[Flubsound.NTamd64.10.0...19041]
%DeviceDesc% = FlubVA, ROOT\FlubsoundVirtualAudio   ; root-enumerated

[FlubVA.NT]
Include   = ks.inf, wdmaudio.inf
Needs     = KS.Registration, WDMAUDIO.Registration
CopyFiles = FlubVA.CopyFiles
AddReg    = FlubVA.AddReg, FlubVA.PinNames     ; MediaCategories names "Flubsound Game" ...

[FlubVA.NT.Interfaces]
AddInterface = %KSCATEGORY_AUDIO%,    %KSNAME_WaveGame%, FlubVA.I.WaveGame
AddInterface = %KSCATEGORY_RENDER%,   %KSNAME_WaveGame%, FlubVA.I.WaveGame
AddInterface = %KSCATEGORY_REALTIME%, %KSNAME_WaveGame%, FlubVA.I.WaveGame
AddInterface = %KSCATEGORY_AUDIO%,    %KSNAME_TopoGame%, FlubVA.I.TopoGame
AddInterface = %KSCATEGORY_TOPOLOGY%, %KSNAME_TopoGame%, FlubVA.I.TopoGame
; ... same for Music, Chat, System (render) and Mic (KSCATEGORY_CAPTURE)

[FlubVA.NT.Services]
AddService = FlubVirtualAudio, 0x00000002, FlubVA.Service

[FlubVA.Service]
ServiceType   = 1                           ; kernel driver
StartType     = 3                           ; demand (PnP)
ErrorControl  = 1
ServiceBinary = %13%\FlubVirtualAudio.sys
```

Installation, done by the elevated MSI/WiX installer through a custom
action:

1. `pnputil /add-driver FlubVirtualAudio.inf` stages the signed package in
   the Driver Store.
2. The installer creates the root devnode as devcon does:
   `SetupDiCreateDeviceInfoW` → `SetupDiSetDeviceRegistryPropertyW
   (SPDRP_HARDWAREID, "ROOT\FlubsoundVirtualAudio")` →
   `SetupDiCallClassInstaller (DIF_REGISTERDEVICE)` →
   `UpdateDriverForPlugAndPlayDevicesW`.
3. It writes `EnabledEndpoints`. It does **not** change the default device;
   there is no documented API for that, and the choice belongs to the user.

Upgrades bump `DriverVer`. Keep the KS reference strings and pin ids stable:
the endpoint id is derived from them, so the user's per-app routing and
volume settings survive updates.

Uninstall: `pnputil /remove-device` on the devnode, then
`pnputil /delete-driver oemNN.inf /uninstall`.

## 8. Signing and HVCI

- **Development.** Test-signed builds, only on dedicated VMs
  (`bcdedit /set testsigning on`, Secure Boot off). Anti-cheat software
  refuses to run games in test mode, so gaming QA always uses signed builds.
- **Release.** Windows 10 1607+ with Secure Boot loads only
  Microsoft-signed new kernel drivers; cross-signing is no longer possible.
  1. Buy an **EV code-signing certificate** (HSM or token). Partner Center
     requires it both to register the company and to sign submissions.
  2. Register in **Partner Center**, hardware program.
  3. Build x64 and ARM64. Create the `.cat` with Inf2Cat, put the package in
     a CAB, and sign the CAB with the EV certificate.
  4. Submit for **attestation signing** (Windows 10/11 client). Microsoft
     returns the package with its signature, usually within hours.
  5. Sign the MSI with the same certificate and an RFC 3161 timestamp.
  - Attestation limits: the signature is not accepted on Windows Server,
    and Windows Update distribution comes with extra Partner Center
    restrictions (check the current shipping-label rules before relying on
    it). Server support, broad WU distribution and the "Windows compatible"
    listing need full **HLK** certification (plan it for v2; the HLK audio
    tests need a dedicated lab).
- **HVCI / Memory integrity** (on by default on new Windows 11 installs):
  - allocate only NX memory (`ExAllocatePool2 (POOL_FLAG_NON_PAGED)`, MDL
    pages mapped with `MdlMappingNoExecute`);
  - no RWX sections and no dynamic code; link with `/CETCOMPAT` (Windows 11
    kernel shadow stacks);
  - validate with Driver Verifier's code-integrity option and the HLK
    "HyperVisor Code Integrity Readiness" test.
- **Never become a BYOVD primitive.** The MAP IOCTL only maps the driver's
  own dedicated pages, never caller-chosen addresses or physical memory. All
  lengths and indices are validated. No kernel pointer is ever returned.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Kernel bug → bugcheck on customers' machines | Minimal kernel code (no DSP), SDV + CodeQL + Driver Verifier in CI, IOCTL fuzzing, 72 h soak tests with stream churn and sleep/resume |
| Shared mapping leaks other apps' audio to an unprivileged process | ACL on the control device, one mapper per endpoint, mappings bound to the handle, dedicated zeroed pages only |
| PortCls rejects a driver-owned MDL (§4.1) | Spike early; fallback is per-stream buffers with deferred free and `Generation` remap |
| audiodg behaviour changes in a Windows feature update (packet timing, position checks) | CI VMs on the Insider Dev/Beta rings, telemetry of glitch counters (`Overruns`/`Underruns`) |
| Clock-slaving DLL instability → audible drift or pitch wobble | Conservative DLL bandwidth, bounded rate correction (±500 ppm), watchdog fallback, ASRC always available |
| Kernel anti-cheat (Vanguard, EAC, BattlEye, FACEIT) flags the driver | Attestation/WHQL signing only, no generic memory IOCTLs, proactive allow-listing with vendors |
| Signing cost and lead time (EV cert, Partner Center) | Start the company registration during M1; attestation turnaround is hours |
| Endpoint clutter and confusion (5 new devices) | Endpoints can be enabled individually; clear names; installer never touches defaults |
| ARM64 Windows (Snapdragon X) | Build and sign ARM64 from day one; the header uses fixed-width types only |
| MS-PL obligations for SYSVAD-derived files | Keep notices; licence review before release |

## 10. Milestones

1. **M1.** SimpleAudioSample fork with one render endpoint, test-signed. The
   engine consumes it through WASAPI loopback, which validates the INF,
   naming and formats.
2. **M2.** Five endpoints, packet mode, low-latency packet constraints, jack
   descriptions.
3. **M3.** Control device, the MAP IOCTLs and the zero-copy engine path,
   using the free-running clock plus the engine's ASRC.
4. **M4.** Engine-slaved clock with the DLL, the watchdog and fallback.
5. **M5.** HVCI/CET readiness, attestation signing, MSI installer, uninstall
   and upgrade tests.
