# tools/neural — training the neural voice cleanup model

Flubsound's first neural model, the experimental **neural voice cleanup** on the Chat strip
(docs/03 §16, docs/09 §1.1, docs/11 E35), is trained here from scratch, with **numpy only**
(no PyTorch, no downloads, no recorded data) and run in the app by the in-house TinyNet
runtime (`core/include/flub/neural/TinyNet.h`).

| File | What it is |
|---|---|
| `fvdsp.py` | The signal processing the model sees: feature set 1 (48 kHz, 5 ms hop, 10 ms Vorbis window, 512-point FFT, 22 band log-energies + voicing) and the BandGains renderer. The numpy twin of `BandGains.h`, `VoiceCleanupRunner.cpp` and `AsyncModelProcessor`'s renderer. |
| `fvsynth.py` | Synthetic data: a source-filter speech synthesiser (male / female / child speakers, phrases of syllables, formants, fricatives, plosives, prosody, jitter, shimmer) and noises (white, pink, brown, fan + mains hum, keyboard typing, babble of 3–8 voices), mixed at −5 … 20 dB SNR with random EQ, levels and codec band limits. |
| `tinynet.py` | A tiny network library: Dense, causal Conv1D and GRU layers with forward / backward (BPTT), Adam, int8 row quantisation, the `.fnn` file writer and a frame-by-frame reference inference. |
| `train_voice_cleanup.py` | The pipeline: `generate`, `train`, `export`, `evaluate`, `card` (or `all`). |
| `renderer_aliasing.py` | Measures the BandGains renderer's time aliasing (its 512-point circular convolution against the linear one with the same response; docs/03 §16.3): `python tools/neural/renderer_aliasing.py`. |

## Reproduce

```
python tools/neural/train_voice_cleanup.py all --work <scratch folder> [--cli build\tools\flubsound-cli\flubsound-cli.exe]
```

* `generate` writes 2 400 training and 96 validation clips of 6 s (4 h of audio) as features and
  targets into the work folder (seeds 100000 + k and 200000 + k; about 10 min on 12 processes).
* `train` runs synchronous data-parallel BPTT (2 500 Adam steps of 192 sequences x 400 frames,
  cosine learning rate from 2e-3), keeping the state with the best validation loss.
* `export` writes `presets/neural/voice-cleanup.fnn` (int8 weights), its embedded copy
  `core/src/neural/VoiceCleanupModelData.cpp`, the C++ tests' reference outputs
  `tests/neural_reference_data.h` and the quality-floor clip `tests/data/neural/voice-cleanup-clip.wav`.
* `evaluate` scores the held-out test set (seeds 300000 + k: 6 noise types x 6 SNRs x 3 clips of
  8 s, plus 6 nearly clean clips). With `--cli` it also renders the same files through the C++
  chain: the spectral noise gate alone (default and 30 dB reduction) and the C++ model
  (`--neural voice-cleanup`), and scores them the same way.
* `card` writes `presets/neural/voice-cleanup.model.json` (architecture, data, training, metrics).

`--quick` is a smoke run (tiny data, 40 steps). The run is seeded end to end; the same numpy
version gives the same files. The model file, the embedded copy and the reference data must be
regenerated together (`tests/test_tinynet.cpp` checks that the embedded copy is the file).

## The model file (`.fnn`, format 1)

A 64-byte header (`FLUBTNET`, format version, header size, payload size, CRC-32 of the payload,
model version, feature set, sample rate, frame size, input size, layer and output counts,
parameter count) and a payload of layer records (type, activation, size, kernel, input tensors,
weights as float32 or int8 rows with float32 scales, float32 biases), the output tensor list and
the model's name. `TinyNet::load` refuses anything it cannot run exactly (see `TinyNet.h`).
