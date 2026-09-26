# 09 — Future Expansion Roadmap (Workflow 6)

Everything here builds on two architectural properties that already exist:
1. A framework-free, allocation-free DSP core with a uniform `Processor` contract (`prepare / reset / process / latencySamples`).
2. A parameter and macro layer that decouples *what the user asks for* from *how modules achieve it*.

New capabilities slot in as processors, as macro-driven control signals, or as new hosts of the same core.

---

## 1. Neural enhancements

### 1.1 How a neural module plugs in (architecture already prepared)

Neural inference is neither bounded in time nor small enough for a 2–3 ms audio callback on every machine. Flubsound therefore **never runs a model on the audio thread**. The pattern is an asynchronous processor with a fixed latency contract:

```
audio thread                               inference worker thread
─────────────                              ───────────────────────
process(block):                            loop:
  push block -> inRing (SPSC)       ─────►   wait for a full frame (e.g. 10 ms)
  pop  result <- outRing (SPSC)     ◄─────   features -> ONNX Runtime -> result
  if result missing (deadline miss):         push result frame
     use fallback (previous gains /
     dry, crossfaded) and count it
  apply result (gains/mask) to the
  block delayed by fixed L samples
latencySamples() == L  (constant)
```

Design rules:
- **Fixed latency `L`** (for example one model frame plus one frame of safety) is reported through `latencySamples()`. Chain compensation, bypass slots and profiles work unchanged.
- **Graceful degradation.** A deadline miss falls back to the last good control frame, or crossfades to dry. It never blocks, and it increments a telemetry counter shown in the UI.
- **Prefer models that output control signals** (per-band gains, masks, detection probabilities) over raw audio. The existing DSP renders the audio, which keeps artefacts bounded and the ceiling guaranteed by the limiter.
- **Runtime:** ONNX Runtime with DirectML (Windows), CoreML (macOS) and CPU (all) execution providers. Models are shipped with a manifest (version, sample rate, frame size, input/output tensors, licence).
- **Latency profiles** decide eligibility. Heavy models are allowed only in *Quality* (music) or offline batch; *Low Latency* gaming only gets models that fit in ≤ 1 frame.
- **Privacy:** on-device only. No audio leaves the machine.

Skeleton type (planned `core/include/flub/neural/AsyncModelProcessor.h`):

```cpp
class AsyncModelProcessor : public Processor
{
public:
    explicit AsyncModelProcessor (std::unique_ptr<ModelRunner> runner, int frameSize, int safetyFrames);
    void prepare (const ProcessSpec&) override;   // starts worker, allocates rings
    void reset() noexcept override;               // clears rings and the fallback state
    void process (const AudioBlock&) noexcept override; // push/pop rings only
    int latencySamples() const noexcept override { return frameSize * (1 + safetyFrames); }
    const char* name() const noexcept override { return "Neural"; }
    uint64_t getDeadlineMisses() const noexcept;
};
```

### 1.2 Candidate neural features (prioritised)

| # | Feature | Mode | Model idea | Output | Notes |
|---|---|---|---|---|---|
| N1 | **Voice-chat noise suppression** (mic + incoming chat) | Gaming | RNNoise (BSD) or DeepFilterNet (MIT/Apache-2.0) class models, 10 ms frames | Per-band gains / deep-filter coefficients | Real-time proven on CPU; the Chat strip only. |
| N2 | **Game event detection** (footsteps, reloads, gunfire, voices) | Gaming | Small CNN on log-mel frames (~10 ms hop) | Class probabilities → drive dynamic-EQ ranges and upward compression | The "footsteps" macro becomes content-aware instead of spectral-only. |
| N3 | **Music source separation** for remix-style enhancement (vocal clarity, bass/drum balance, stereo → immersive upmix) | Music (Quality / batch) | Hybrid spectrogram/waveform separators (Demucs family) for batch; lightweight real-time variants | Stems or masks | Batch first. Real-time only on capable GPUs. |
| N4 | **Neural bandwidth extension** for low-bitrate streams (voice chat, 128 kbps music) | Both | Spectral band replication network | High-band envelope → exciter drive | Replaces the static air exciter when confident. |
| N5 | **Learned loudness/tonal "auto-master"** | Music | Regressor from track features to macro settings | Macro values | Suggests Boost/macro settings per track; the user stays in control. |
| N6 | **Personalised HRTF** from ear photos / anthropometrics | Gaming | HRTF selection/synthesis model | HRIR set (SOFA) | Feeds the measured-HRIR renderer. |
| N7 | **Hearing-profile personalisation** from an in-app hearing test | Both | Audiogram → compensation curve | EQ + compression settings | Presented as comfort, not as a medical device. |

---

## 2. Spatial & device features

| Feature | Description |
|---|---|
| Measured-HRIR renderer (HQ) | SOFA loading, resampling, partitioned FFT convolution, head-size personalisation; the parametric model remains the fallback. The core already renders a supplied HRIR set by direct-form convolution (`HeadphoneVirtualizer::setHrirSet`, capped at 1024 taps, ≈ 21 ms at 48 kHz); nothing loads one yet. |
| Head tracking | Webcam, AirPods/Bluetooth IMU or IMU dongle → rotate the virtual speaker field (virtualiser angles become dynamic). |
| Speaker crosstalk cancellation | 3D gaming on laptop and desktop speakers (XTC with regularisation). |
| Headphone correction database | Target-curve EQ presets per headphone model (licence-checked datasets). |
| Room correction | Microphone measurement → correction filters for speakers (min-phase + optional linear-phase in Quality profile). |
| Stereo → 7.1 upmix | Music and older games into the spatial renderer (ambience extraction via decorrelation / PCA). |
| Object-audio passthrough | Investigate Windows Spatial Sound (`ISpatialAudioClient`) provider integration; this requires Microsoft partnership. |

---

## 3. Platform & host expansion

| Feature | Description |
|---|---|
| **APO Lite mode** (Windows) | Host `flub_core` inside an APO for per-endpoint processing without the virtual driver: lowest possible latency, no per-app routing. |
| **Native PipeWire filter node** (Linux) | `pw_filter` hosting the chain directly in the PipeWire graph. |
| **Engine as a service** | Split the audio engine into a background service process with shared-memory IPC to the GUI. The GUI can crash or update without interrupting audio. The IPC layer is a Rust candidate (see `02-tech-stack.md`). |
| **Double-buffered engine swap** | Crossfaded replacement of the whole `MixEngine` on latency-profile or device changes (no dropout), with RCU-style deferred reclamation. |
| Plug-in hosting | Load VST3/CLAP plug-ins into a strip ("FX slot") after the maximizer's pre-stage, sandboxed in a separate process. |
| Android | System-wide effects via Android's global `DynamicsProcessing` effect API (EQ, MBC, limiter), with a reduced feature set. |
| Consoles / TVs | Hardware-partner SDK licensing of `flub_core` (DSP only). |

---

## 4. Ecosystem & product

| Feature | Description |
|---|---|
| Cloud presets & community | Share and rate presets (JSON already versioned and key-based). Signed "verified" presets from game studios and artists. |
| Game auto-profiles | Curated per-title profiles that switch automatically by executable, plus studio partnerships. |
| Automation API | Local WebSocket/gRPC API for Stream Deck, macros and companion apps (mode, preset, boost, bypass). |
| Streaming / OBS | Separate "stream mix" output without personal enhancements (the audience hears the game, not your bass boost). |
| Microphone chain | Gate, NR (N1), EQ, compressor and limiter for the voice-chat Mic endpoint. |
| Haptics | Route low-frequency impact (explosions) to haptic devices (bass shakers, controllers). |
| Accessibility | Visual sound radar for the hearing impaired: direction and loudness of game events from the 7.1 stream + N2 detection. |

---

## 5. Sequencing (post-1.0)

| Release | Headline items |
|---|---|
| 1.1 | Measured-HRIR renderer, headphone correction database, native PipeWire node, double-buffered engine swap |
| 1.2 | N1 voice-chat denoise, microphone chain, OBS stream mix, automation API |
| 1.3 | N2 content-aware gaming enhancement, game auto-profiles, sound radar |
| 1.4 | APO Lite mode, speaker XTC, room correction |
| 2.0 | Engine-as-a-service, N3 separation-based music enhancement, head tracking, cloud presets |
