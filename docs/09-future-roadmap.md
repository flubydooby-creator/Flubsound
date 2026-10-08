# 09 — Future Expansion Roadmap (Workflow 6)

Everything here builds on two architectural properties that already exist:
1. A framework-free, allocation-free DSP core with a uniform `Processor` contract (`prepare / reset / process / latencySamples`).
2. A parameter and macro layer that decouples *what the user asks for* from *how modules achieve it*.

New capabilities slot in as processors, as macro-driven control signals, or as new hosts of the same core.

---

## 1. Neural enhancements

### 1.1 How a neural module plugs in (framework, TinyNet runtime and the first model)

Neural inference is neither bounded in time nor small enough for a 2–3 ms audio callback on every machine. Flubsound therefore **never runs a model on the audio thread**. The core implements this as an asynchronous processor with a fixed latency contract, `flub::AsyncModelProcessor` (`core/include/flub/neural/`):

```
audio thread: process(block)                   inference worker (one thread per processor)
────────────────────────────                   ───────────────────────────────────────────
copy input into the current model frame        sleep until woken (100 ms safety timeout)
frame full -> input FrameQueue (SPSC)   ─────►   skip frames whose deadline has passed
end of the call: wake the worker        ─────►   (a WakeEvent; one non-blocking OS call)
at every frame boundary:                         ModelRunner::run(frame) -> control frame
  pop the result for the frame that     ◄─────   push the control frame (result FrameQueue)
  now reaches the output; if it is missing
  or failed: keep the last good controls,
  after K bad frames ramp to neutral
output = input delayed by L × smoothed controls
latencySamples() == L == frameSize × (1 + safetyFrames)   (constant; BandGains: frameSize × (2 + safetyFrames))
```

What exists:
- **`ModelRunner`** (`neural/ModelRunner.h`) is the model interface. `describe()` returns the frame size, the input channel count (1 = mono downmix, or planar channels), the number of floats in the control frame the model outputs, the control kind (and, for band gains, the FFT size and band centres) and the sample rate the model expects. `run (inFrame, outControls)` returns `false` on failure. The runner is prepared off the audio thread and only ever runs on the worker (in offline mode, on the rendering thread). `neural/ReferenceRunners.h` holds stand-ins with a known answer: identity, constant gain and always failing. `neural/VoiceCleanupRunner.h` is the first real model (below).
- **TinyNet, the inference runtime** (`neural/TinyNet.h`, `core/src/neural/TinyNet.cpp`; [03 §16](03-dsp-design.md#16-neural-voice-cleanup-experimental)). In-house, plain C++, no third-party dependency: Dense, causal Conv1D and GRU layers, linear / ReLU / tanh / sigmoid, skip connections (a layer reads the concatenation of up to four earlier tensors), several outputs. A versioned binary format (`.fnn`: a 64-byte header with the format and model versions, feature set, sample rate, frame size, input size, layer / output / parameter counts and a CRC-32 of the payload; float32 or per-row int8 weights) that `load()` validates completely, refusing truncated, corrupt or hostile files with a reason. `load()` allocates everything; `run()` and `reset()` allocate nothing, take no lock, do a fixed amount of work and give the same bits on every run (`FLUB_NONBLOCKING`). Training, export and the numpy reference that the runtime is tested against are in `tools/neural/` (numpy only).
- **The first model: neural voice cleanup** (`neural/VoiceCleanupRunner.h`, weights in `presets/neural/voice-cleanup.fnn`, embedded in `core/src/neural/VoiceCleanupModelData.cpp`; [03 §16](03-dsp-design.md#16-neural-voice-cleanup-experimental)). N1 for incoming chat: 5 ms frames of the mono downmix at 48 kHz, 22 band log-energies and a voicing strength in, 22 band gains and a voice-activity probability out, from a 51 k-parameter network (a causal convolution and two GRUs) trained in-house on 4 h of synthetic speech in noise. 27 µs per frame on one core. On the held-out synthetic set it beats the spectral gate where a chat denoiser matters (−19.9 vs −6.1 dB between words, +4.7 vs +3.5 dB SNR, typing and babble) and is worse on clean speech (−0.24 dB, 0.55 dB per band); **experimental**, off by default, synthetic data only. The app runs it on the Chat strip (Settings › Processing › *Voice chat*); the CLI with `--neural voice-cleanup`.
- **Fixed latency `L`** = `frameSize × (1 + safetyFrames)` (default one safety frame; `frameSize × (2 + safetyFrames)` for band gains, whose overlap-add adds a frame) is reported through `latencySamples()`. The control computed from input frame *k* is applied to exactly the samples of frame *k*, so the worker has `safetyFrames` frame periods to deliver. That budget must cover one host block, the model's worst-case run time and the worker's wake-up latency (below). The host block is a hard limit: a result can only be picked up by a later `process()` call than the one that submitted its frame, so with blocks longer than `safetyFrames × frameSize` about (block − `safetyFrames × frameSize`) / block of the frames miss even with an instant model (53 % for 480-sample frames, one safety frame and 1024-sample buffers); `getMaxBlockSizeWithoutMisses()` reports the limit, and the chain enforces it (below).
- **Waking the worker** (since 2026-10-08, [11 E35](11-enhancement-report.md#e35)). The worker sleeps until the audio thread has queued work. At the end of each `process()` call that queued a frame, the audio thread signals a `WakeEvent` (`neural/WakeEvent.h`): an atomic exchange and, only when the worker is asleep, one OS call that readies it: `SetEvent` on an auto-reset event (Windows), a futex wake on the state word (Linux), `semaphore_signal` on a Mach semaphore (macOS). None of them takes a user-space lock or waits for another thread, and no wake-up can be lost (the worker announces its sleep on the same atomic the audio thread exchanges). RealtimeSanitizer cannot tell such a wake from a blocking call (on Linux it intercepts `syscall()`), so that one call runs under `__rtsan::ScopedDisabler`, on purpose: `tests/test_rtsan.cpp` shows RTSan stops a bare futex wake in a nonblocking function and lets `WakeEvent::signal()` through, and every neural test runs it under RTSan. The worker's sleep has a 100 ms safety timeout (`workerTimeoutMicroseconds`), and a stop request wakes it at once. So it wakes once per host block that completes a frame (100 times a second with 480-sample blocks) and about 10 times a second when idle, instead of every frame period / 8 (625 µs for the voice cleanup's 5 ms frames: 1 600 times a second where the OS timer is precise, macOS with the time-constraint policy and Linux; about 500 on Windows with its 1 ms timer), and it answers within the OS's wake-up latency instead of half a poll interval plus timer slack. Measured in the paced VoiceCleanup case (480-sample blocks, polling → woken; every leg in [11 E35](11-enhancement-report.md#e35)): on the macOS CI runner 1 438 – 1 523 → 100 wake-ups per second, the worker's response median 0.35 – 0.41 → 0.07 ms, its thread's CPU time 28 – 32 → 10 – 12 ms per second (the model about 10); on the owner's PC (Windows) 460 – 563 → 100 wake-ups per second, response median 1.0 – 1.5 → 0.08 – 0.11 ms, CPU 13.9 – 18.1 → 11.6 – 13.5 ms per second. The audio thread pays the OS call: `signal()` 0.3 – 2.8 µs median. The earlier polling (`AsyncModelConfig::workerPolls`, `workerPollMicroseconds`: a frame period / 8, clamped to 100 µs – 1 ms) remains as the fallback where the OS object cannot be created; `getWorkerWake()` reports which is in use and `getWorkerWakeups()` counts the worker's sleeps that ended.
- **Worker scheduling:** on macOS the worker gives itself the Mach time-constraint policy that Core Audio's I/O threads have (period one model frame, computation half of it), so it gets a CPU at once when woken. The reason was the polling worker: macOS coalesces the timers of every thread that is not real time whatever its QoS class, and on the GitHub macOS runner a 625 µs poll sleep lasted about 5.7 ms and the worker's response reached 8.5 ms of a 10 ms budget at the 99th percentile; with the policy, 0.64 ms and about 1 ms. Windows and Linux keep the OS default. `getWorkerScheduling()` reports what the worker got. On every OS the model runs with FTZ / DAZ (`ScopedNoDenormals`), offline too.
- **Graceful degradation.** A result that has not arrived when its frame reaches the output is a deadline miss; a result whose `run()` returned `false` (or threw), or that holds a NaN / Inf, is a model failure. Both keep the last good control frame; after `fallbackAfterFrames` (K) consecutive bad frames the controls ramp to neutral (unity gain, i.e. the delayed dry signal). Every control change is a linear ramp (`controlRampMs`), so no transition clicks. Late results are dropped, and stale input frames are skipped, so the worker catches up at once after a stall. Outside offline mode `process()` never blocks, allocates or locks. `reset()` is RT-safe and does not stop the worker. The destructor joins the worker. `getDeadlineMisses()`, `getModelFailures()` and `getFramesProcessed()` are relaxed atomic counters; `ProcessingChain::getNeuralCounters()` republishes them once per block for any thread, and showing them in the UI is roadmap.
- **Offline rendering** (`AsyncModelConfig::offline`, set by the chain for `ModelContext::Offline`). A batch render calls `process()` back to back, far faster than real time, so a deadline counted in samples would pass before the worker could answer: most frames would fall back to the dry signal, by an amount that depends on thread scheduling. In offline mode there is no worker thread: `process()` runs the model on the calling thread as soon as a frame is complete, so every frame's result is applied to exactly that frame whatever the render speed or block length, there are no deadline misses, and two renders give the same bytes. `process()` then takes as long as the model does, so offline mode is for batch renders only, never an audio callback.
- **Control signals, not audio.** Controls are linear gains, clamped to `[0, maxGain]` (+12 dB by default): broadband (control 0 on every channel), one per output channel, or one per frequency band (`ControlKind::BandGains`, `neural/BandGains.h`). The existing DSP renders the audio, which keeps artefacts bounded and the ceiling guaranteed by the limiter. The band renderer is an STFT on the audio thread: hop = the model's frame, a 2-frame Vorbis power-complementary window zero-padded to the model's FFT size, bin gains interpolated between the band centres with the same triangular weights a model's features use, overlap-add; the window that ends with input frame k gets the gains computed from frame k. It adds one frame to the latency (L = `frameSize × (2 + safetyFrames)`), reconstructs the input at unity gains (null −128.9 dB) and crossfades a gain change over one frame. **Roadmap:** masks (per-bin), and filter-bank renderers with less latency.
- **Latency profiles decide eligibility** (`neural/Eligibility.h`, `isEligible (profile, modelLatencySamples, sampleRate, context)`), in 10 ms reference frames: offline batch rendering and *Quality* accept any model (heavy models are allowed only there); *Balanced* accepts at most 2 frames (one 10 ms frame plus one frame of safety); *Low Latency* accepts at most 1 frame (480 samples at 48 kHz). `ProcessingChain::prepare()` enforces the rule (below).
- **In the chain: an optional neural slot.** `ProcessingChain::setNeuralModel (runner, NeuralSlotConfig)` (non-RT; `clearNeuralModel()` removes it) installs a model; like a latency-profile change it takes effect at the next `prepare()`, and `needsReprepare()` says one is due. The slot is empty by default and is then skipped, so the chain's latency and output are exactly what they are without it. `prepare()` puts the model in only if `isEligible (profile, L, sampleRate, context)` holds, its sample rate matches and, in the `Realtime` context, `maxBlockSize` is at most `safetyFrames × frameSize` (above); otherwise the model stays installed but out of the chain (no worker thread, no latency) until a later `prepare()` that allows it, and `getNeuralStatus()` reports the reason (`Ineligible`, `SampleRateMismatch`, `InvalidModel`, `PrepareFailed`, `BlockTooLarge`; `neuralSlotReason()` has a UI sentence for each). A batch renderer sets the config's `context` to `Offline`: any model and any block length are accepted, and the processor runs in offline mode (above). An active model adds exactly its `L` to `getLatencySamples()`. **Placement:** after the spectral gate, before the EQ, so every dynamics stage comes after it: the compressor and the maximizer act on what the controls did, and the true-peak limiter still guarantees the ceiling whatever gain the model applies (up to `maxGain`). It also sits after the gate, whose noise-floor tracking would otherwise follow the model's frame-rate gain, and before the tonal, saturation and width stages, so the model sees the source signal it was trained on. The slot is one more entry in the chain's fixed `ModuleSlot` array, so `setNeuralBypass()` fades the model out and back in without changing the latency, and a NaN block resets it with the rest of the chain.
- Tests: `tests/test_tinynet.cpp` (*TinyNet: …*: layer math within 2.4 · 10⁻⁷ of the numpy reference, corrupt files, allocation-free and deterministic inference; *VoiceCleanup: …*: the embedded model, features and gains against numpy), `tests/test_voice_cleanup.cpp` (*Neural BandGains: …*: unity null, gains on their own window, fallback; *VoiceCleanup: …*: the real worker with 480-sample blocks, waited for and paced at real time (woken, and polling as the reference), eligibility in the chain, cost, a quality floor; invalid band layouts), `tests/app/test_app_neural_cleanup.cpp` (the app's switch). `tests/test_neural.cpp` (*Neural: …*) drives the real worker thread with scripted stand-in models (latency and sample-exact delay, frame alignment, offline mode with no waiting and long blocks, deadline misses counted exactly, hold-then-fade fallback without clicks, failures, reset, allocation-free `process()`, worker lifecycle, eligibility, the `WakeEvent` hand-overs and the worker's wake-ups idle, per block and at a stop); `tests/test_neural_slot.cpp` (*NeuralSlot: …*) runs models through the full chain (no model: the reference latencies and bit-identical output; an identity model adds exactly `L` and delays the output by `L`; a 2-frame model is ineligible in Low Latency; sample-rate mismatch, invalid description and failing `prepare()`; a buffer longer than the safety frames (`BlockTooLarge`); an Offline render with no waiting that applies each frame's own gain, reproducibly; the true-peak ceiling with −6 dB and +12 dB models on hot material; latency-constant bypass; allocation-free `process()`; `clearNeuralModel()` restores the latency); `tests/test_rtsan.cpp` checks that `process()` and `reset()` are `FLUB_NONBLOCKING`, and so are `WakeEvent::signal()` and the chain's neural bypass, status and counter accessors, and that RTSan stops a bare futex wake but not the exempted `signal()`.

Still roadmap:
- **Runtime for large models:** ONNX Runtime with DirectML (Windows), CoreML (macOS) and CPU (all) execution providers, as another `ModelRunner`, for models TinyNet's layer set cannot express (DeepFilterNet-class deep filters, separators). TinyNet's `.fnn` header is the manifest for its own models; a signed manifest with a licence field is roadmap ([11 E35](11-enhancement-report.md#e35)).
- **Host integration.** The app installs one model, the voice cleanup on the Chat strip, through a host setting (no parameter or preset key); the CLI renders with it; the plug-in installs none. The chain runs one model per strip, each processor with its own worker thread, woken by the audio thread (a shared scheduler is roadmap). The slot's misses and failures appear only in the Settings status line, not in a meter.
- **Privacy:** on-device only. No audio leaves the machine.

Public API (`core/include/flub/neural/AsyncModelProcessor.h`, abridged):

```cpp
class AsyncModelProcessor final : public Processor
{
public:
    explicit AsyncModelProcessor (std::unique_ptr<ModelRunner> runner, const AsyncModelConfig& config = {});
    void prepare (const ProcessSpec&) override;                              // allocates the queues, starts the worker
    void reset() noexcept FLUB_NONBLOCKING override;                         // clears delay + controls; worker keeps running
    void process (const AudioBlock&) noexcept FLUB_NONBLOCKING override;     // frame queues and one wake-up, never waits (offline mode: runs the model)
    int latencySamples() const noexcept override;                            // frameSize * (1 + safetyFrames); BandGains (2 + safetyFrames)
    const char* name() const noexcept override { return "Neural"; }
    uint64_t getDeadlineMisses() const noexcept;
    uint64_t getModelFailures() const noexcept;
    uint64_t getFramesProcessed() const noexcept;
    NeuralWorkerWake getWorkerWake() const noexcept;                         // Signal (woken by process()) / Poll (fallback) / None
    uint64_t getWorkerWakeups() const noexcept;                              // the worker's sleeps that ended (the energy measure)
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
| N1 | **Voice-chat noise suppression** (mic + incoming chat) | Gaming | RNNoise (BSD) or DeepFilterNet (MIT/Apache-2.0) class models, 10 ms frames | Per-band gains / deep-filter coefficients | Real-time proven on CPU; the Chat strip only. **A first in-house RNNoise-style model ships, experimental and off by default** ([03 §16](03-dsp-design.md#16-neural-voice-cleanup-experimental)): incoming chat only, synthetic training data. |
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
| **Double-buffered engine swap** | Done in the desktop app for latency-profile, strip-layout and neural-model changes (crossfaded, RCU-style deferred reclamation, [01 §3](01-architecture.md#3-process--thread-model)). Remaining: device-format changes restart the device, so the new engine fades in from silence rather than crossfading. |
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
