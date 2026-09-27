# 09 — Future Expansion Roadmap (Workflow 6)

Everything here builds on two architectural properties that already exist:
1. A framework-free, allocation-free DSP core with a uniform `Processor` contract (`prepare / reset / process / latencySamples`).
2. A parameter and macro layer that decouples *what the user asks for* from *how modules achieve it*.

New capabilities slot in as processors, as macro-driven control signals, or as new hosts of the same core.

---

## 1. Neural enhancements

### 1.1 How a neural module plugs in (framework in the core; no runtime or model yet)

Neural inference is neither bounded in time nor small enough for a 2–3 ms audio callback on every machine. Flubsound therefore **never runs a model on the audio thread**. The core implements this as an asynchronous processor with a fixed latency contract, `flub::AsyncModelProcessor` (`core/include/flub/neural/`):

```
audio thread: process(block)                   inference worker (one thread per processor)
────────────────────────────                   ───────────────────────────────────────────
copy input into the current model frame        poll the input FrameQueue (bounded sleep)
frame full -> input FrameQueue (SPSC)   ─────►   skip frames whose deadline has passed
at every frame boundary:                         ModelRunner::run(frame) -> control frame
  pop the result for the frame that     ◄─────   push the control frame (result FrameQueue)
  now reaches the output; if it is missing
  or failed: keep the last good controls,
  after K bad frames ramp to neutral
output = input delayed by L × smoothed controls
latencySamples() == L == frameSize × (1 + safetyFrames)   (constant)
```

What exists:
- **`ModelRunner`** (`neural/ModelRunner.h`) is the model interface. `describe()` returns the frame size, the input channel count (1 = mono downmix, or planar channels), the number of floats in the control frame the model outputs, the control kind and the sample rate the model expects. `run (inFrame, outControls)` returns `false` on failure. The runner is prepared off the audio thread and only ever runs on the worker (in offline mode, on the rendering thread). `neural/ReferenceRunners.h` holds stand-ins with a known answer: identity, constant gain and always failing.
- **Fixed latency `L`** = `frameSize × (1 + safetyFrames)` (default one safety frame) is reported through `latencySamples()`. The control computed from input frame *k* is applied to exactly the samples of frame *k*, so the worker has `safetyFrames` frame periods to deliver. That budget must cover one host block, the model's worst-case run time and the worker's poll interval. The host block is a hard limit: a result can only be picked up by a later `process()` call than the one that submitted its frame, so with blocks longer than `safetyFrames × frameSize` about (block − `safetyFrames × frameSize`) / block of the frames miss even with an instant model (53 % for 480-sample frames, one safety frame and 1024-sample buffers); `getMaxBlockSizeWithoutMisses()` reports the limit, and the chain enforces it (below). The poll interval is a frame period / 8 by default, clamped to 100 µs – 1 ms. The worker polls instead of being woken: a wake-up (`std::atomic::notify`, a semaphore) would be a system call on the audio thread and needs macOS 11 in libc++. The cost is idle wake-ups (about 1 000/s for 10 ms frames) and up to one poll interval plus OS timer slack of extra latency, which the safety frames absorb.
- **Graceful degradation.** A result that has not arrived when its frame reaches the output is a deadline miss; a result whose `run()` returned `false` (or threw), or that holds a NaN / Inf, is a model failure. Both keep the last good control frame; after `fallbackAfterFrames` (K) consecutive bad frames the controls ramp to neutral (unity gain, i.e. the delayed dry signal). Every control change is a linear ramp (`controlRampMs`), so no transition clicks. Late results are dropped, and stale input frames are skipped, so the worker catches up at once after a stall. Outside offline mode `process()` never blocks, allocates or locks. `reset()` is RT-safe and does not stop the worker. The destructor joins the worker. `getDeadlineMisses()`, `getModelFailures()` and `getFramesProcessed()` are relaxed atomic counters; `ProcessingChain::getNeuralCounters()` republishes them once per block for any thread, and showing them in the UI is roadmap.
- **Offline rendering** (`AsyncModelConfig::offline`, set by the chain for `ModelContext::Offline`). A batch render calls `process()` back to back, far faster than real time, so a deadline counted in samples would pass before a polling worker could answer: most frames would fall back to the dry signal, by an amount that depends on thread scheduling. In offline mode there is no worker thread: `process()` runs the model on the calling thread as soon as a frame is complete, so every frame's result is applied to exactly that frame whatever the render speed or block length, there are no deadline misses, and two renders give the same bytes. `process()` then takes as long as the model does, so offline mode is for batch renders only, never an audio callback.
- **Control signals, not audio.** Controls are linear gains, clamped to `[0, maxGain]` (+12 dB by default): broadband (control 0 on every channel) or one per output channel. The existing DSP renders the audio, which keeps artefacts bounded and the ceiling guaranteed by the limiter. **Roadmap:** per-band gains and masks need a band-split renderer (a new `ControlKind`).
- **Latency profiles decide eligibility** (`neural/Eligibility.h`, `isEligible (profile, modelLatencySamples, sampleRate, context)`), in 10 ms reference frames: offline batch rendering and *Quality* accept any model (heavy models are allowed only there); *Balanced* accepts at most 2 frames (one 10 ms frame plus one frame of safety); *Low Latency* accepts at most 1 frame (480 samples at 48 kHz). `ProcessingChain::prepare()` enforces the rule (below).
- **In the chain: an optional neural slot.** `ProcessingChain::setNeuralModel (runner, NeuralSlotConfig)` (non-RT; `clearNeuralModel()` removes it) installs a model; like a latency-profile change it takes effect at the next `prepare()`, and `needsReprepare()` says one is due. The slot is empty by default and is then skipped, so the chain's latency and output are exactly what they are without it. `prepare()` puts the model in only if `isEligible (profile, L, sampleRate, context)` holds, its sample rate matches and, in the `Realtime` context, `maxBlockSize` is at most `safetyFrames × frameSize` (above); otherwise the model stays installed but out of the chain (no worker thread, no latency) until a later `prepare()` that allows it, and `getNeuralStatus()` reports the reason (`Ineligible`, `SampleRateMismatch`, `InvalidModel`, `PrepareFailed`, `BlockTooLarge`; `neuralSlotReason()` has a UI sentence for each). A batch renderer sets the config's `context` to `Offline`: any model and any block length are accepted, and the processor runs in offline mode (above). An active model adds exactly its `L` to `getLatencySamples()`. **Placement:** after the spectral gate, before the EQ, so every dynamics stage comes after it: the compressor and the maximizer act on what the controls did, and the true-peak limiter still guarantees the ceiling whatever gain the model applies (up to `maxGain`). It also sits after the gate, whose noise-floor tracking would otherwise follow the model's frame-rate gain, and before the tonal, saturation and width stages, so the model sees the source signal it was trained on. The slot is one more entry in the chain's fixed `ModuleSlot` array, so `setNeuralBypass()` fades the model out and back in without changing the latency, and a NaN block resets it with the rest of the chain.
- Tests: `tests/test_neural.cpp` (*Neural: …*) drives the real worker thread with scripted stand-in models (latency and sample-exact delay, frame alignment, offline mode with no waiting and long blocks, deadline misses counted exactly, hold-then-fade fallback without clicks, failures, reset, allocation-free `process()`, worker lifecycle, eligibility); `tests/test_neural_slot.cpp` (*NeuralSlot: …*) runs models through the full chain (no model: the reference latencies and bit-identical output; an identity model adds exactly `L` and delays the output by `L`; a 2-frame model is ineligible in Low Latency; sample-rate mismatch, invalid description and failing `prepare()`; a buffer longer than the safety frames (`BlockTooLarge`); an Offline render with no waiting that applies each frame's own gain, reproducibly; the true-peak ceiling with −6 dB and +12 dB models on hot material; latency-constant bypass; allocation-free `process()`; `clearNeuralModel()` restores the latency); `tests/test_rtsan.cpp` checks that `process()` and `reset()` are `FLUB_NONBLOCKING`, and so are the chain's neural bypass, status and counter accessors.

Still roadmap:
- **Runtime:** ONNX Runtime with DirectML (Windows), CoreML (macOS) and CPU (all) execution providers, as a `ModelRunner`. Models are shipped with a manifest (version, sample rate, frame size, input/output tensors, licence) that fills `ModelDescription`. No trained model ships.
- **Host integration.** The chain's slot is a host API only: there is no parameter, preset key or UI for it yet, and neither the app nor the plug-in installs a model. The chain runs one model per strip.
- **Privacy:** on-device only. No audio leaves the machine.

Public API (`core/include/flub/neural/AsyncModelProcessor.h`, abridged):

```cpp
class AsyncModelProcessor final : public Processor
{
public:
    explicit AsyncModelProcessor (std::unique_ptr<ModelRunner> runner, const AsyncModelConfig& config = {});
    void prepare (const ProcessSpec&) override;                              // allocates the queues, starts the worker
    void reset() noexcept FLUB_NONBLOCKING override;                         // clears delay + controls; worker keeps running
    void process (const AudioBlock&) noexcept FLUB_NONBLOCKING override;     // frame queues only, never waits (offline mode: runs the model)
    int latencySamples() const noexcept override;                            // frameSize * (1 + safetyFrames)
    const char* name() const noexcept override { return "Neural"; }
    uint64_t getDeadlineMisses() const noexcept;
    uint64_t getModelFailures() const noexcept;
    uint64_t getFramesProcessed() const noexcept;
    int getMaxBlockSizeWithoutMisses() const noexcept;                       // safetyFrames * frameSize (offline: unbounded)
};

// core/include/flub/engine/ProcessingChain.h (neural slot, abridged)
void setNeuralModel (std::unique_ptr<ModelRunner> runner, const NeuralSlotConfig& config = {}); // non-RT; at the next prepare()
void clearNeuralModel();                                                // non-RT; at the next prepare()
NeuralSlotStatus getNeuralStatus() const noexcept;     // Empty / Active / Ineligible / SampleRateMismatch / InvalidModel / PrepareFailed / BlockTooLarge, L
NeuralSlotCounters getNeuralCounters() const noexcept; // deadline misses, model failures, frames (published per block)
void setNeuralBypass (bool bypassed) noexcept;         // ModuleSlot fade; latency unchanged
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
