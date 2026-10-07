// Flubsound Pro - TinyNet: a small, real-time-safe inference runtime for
// frame-by-frame (streaming) neural networks, in plain C++ with no third-party
// dependencies (docs/09 §1.1, docs/03 §16).
//
// Layers (each runs once per model frame; all causal):
//   Dense   y = act (W x + b)                                   W: out x in
//   Conv1D  y_t = act (W [x_{t-k+1} ... x_t] + b) (k frames)     W: out x (k * in), oldest frame first
//   GRU     z = sigm (Wz x + bxz + Uz h + bhz)                   W: 3H x in, U: 3H x H, rows z, r, n
//           r = sigm (Wr x + bxr + Ur h + bhr)
//           n = tanh (Wn x + bxn + r * (Un h + bhn))            ("reset after", as cuDNN / PyTorch)
//           h = (1 - z) * n + z * h
// Activations: linear, ReLU, tanh, sigmoid. Tensor 0 is the model input,
// tensor i the output of layer i - 1; a layer reads the concatenation of up
// to four earlier tensors (skip connections), and the model's outputs are a
// list of tensors.
//
// File format (version 1, little-endian; tools/neural/tinynet.py writes it):
//   header, 64 bytes: "FLUBTNET", formatVersion, headerBytes (64),
//     payloadBytes, payloadCrc32 (CRC-32/IEEE, as zlib.crc32), modelVersion,
//     featureSet, sampleRate, frameSize, numFeatures, numLayers, numOutputs,
//     parameterCount, flags (0), reserved (0)  (all uint32)
//   payload: per layer type, activation, outSize, kernel, numInputs and the
//     input tensor ids (uint32), weightFormat (0 = float32, 1 = int8 with one
//     float32 scale per row: the scales, then the rows, padded to 4 bytes),
//     the matrices (Dense / Conv1D: W; GRU: W, U) and the float32 biases
//     (Dense / Conv1D: b; GRU: bx, bh); then numOutputs output tensor ids;
//     then the name (uint32 length, UTF-8, padded to 4 bytes).
// load() checks everything (sizes, CRC, limits, references to earlier
// tensors only, finite numbers, every byte used exactly once) and refuses a
// file it cannot run exactly, with a reason. It dequantises int8 weights once,
// so inference is float32 only.
//
// Threading: load() is non-RT (it allocates everything the net will ever
// need). reset() and run() allocate nothing, take no lock and do a fixed
// amount of work per call (FLUB_NONBLOCKING: RTSan checks them in its CI
// job); the result depends only on the weights, the inputs since the last
// reset() and the compiler's float arithmetic (fixed summation order, no
// fast-math), so two runs give the same bits. One instance per thread.
#pragma once

#include "flub/common/Realtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flub::nn
{
enum class LayerType : uint32_t
{
    Dense = 1,
    Conv1D = 2,
    Gru = 3
};

enum class Activation : uint32_t
{
    Linear = 0,
    Relu = 1,
    Tanh = 2,
    Sigmoid = 3
};

inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kHeaderBytes = 64;
inline constexpr int kMaxLayers = 32;
inline constexpr int kMaxUnits = 1024;          // outputs of one layer
inline constexpr int kMaxLayerInputs = 4;       // tensors one layer concatenates
inline constexpr int kMaxKernel = 16;           // Conv1D frames
inline constexpr int kMaxFeatures = 1024;       // model inputs
inline constexpr int kMaxOutputs = 8;           // output tensors
inline constexpr uint32_t kMaxParameters = 1u << 22;
inline constexpr uint32_t kMaxNameBytes = 256;

/** The header fields a host checks (sample rate, frame size and feature set
    say which front end the model expects; TinyNet itself does not use them). */
struct ModelInfo
{
    uint32_t formatVersion = 0, modelVersion = 0, featureSet = 0, sampleRate = 0, frameSize = 0;
    uint32_t numFeatures = 0, numLayers = 0, numOutputs = 0, parameterCount = 0;
    std::string name;
};

/** CRC-32 (IEEE 802.3, reflected, as zlib.crc32). */
uint32_t crc32 (const void* data, size_t size) noexcept;

class TinyNet
{
public:
    /** Non-RT. Parses and validates a whole model file held in memory and
        allocates all state. On failure returns false with a reason in
        `error` and leaves the net empty (isLoaded() false). */
    bool load (const void* data, size_t size, std::string& error);

    bool isLoaded() const noexcept { return loaded; }
    const ModelInfo& info() const noexcept { return modelInfo; }
    int numInputs() const noexcept { return loaded ? tensorSize[0] : 0; }
    int numOutputs() const noexcept { return static_cast<int> (outputTensors.size()); }
    int outputSize (int index) const noexcept;

    /** Clears the recurrent state and the convolution history (the next run()
        starts a new stream). */
    void reset() noexcept FLUB_NONBLOCKING;

    /** One model frame: input holds numInputs() floats. Allocation-free. */
    void run (const float* input) noexcept FLUB_NONBLOCKING;

    /** Output `index` of the last run() (outputSize (index) floats). */
    const float* output (int index) const noexcept;

private:
    struct Layer
    {
        LayerType type = LayerType::Dense;
        Activation act = Activation::Linear;
        int out = 0, in = 0, kernel = 1, numInputs = 0;
        std::array<int, kMaxLayerInputs> inputs {};
        size_t w = 0, u = 0, b = 0, bh = 0;  // offsets into `weights`
        size_t state = 0;                    // offset into `state`: GRU h (out) or the Conv1D history ((kernel - 1) * in)
        size_t outOffset = 0;                // offset of the layer's output tensor in `tensors`
    };

    const float* gatherInput (const Layer& layer) noexcept;

    bool loaded = false;
    ModelInfo modelInfo;
    std::vector<Layer> layers;
    std::vector<float> weights;              // every parameter, dequantised
    std::vector<float> tensors;              // tensor 0 (the input) and every layer's output, back to back
    std::vector<size_t> tensorOffset;
    std::vector<int> tensorSize;
    std::vector<float> scratch;              // concatenated inputs + GRU gate pre-activations
    size_t gateOffset = 0;                   // where the gate buffers start in `scratch`
    std::vector<float> state;
    std::vector<int> outputTensors;
};
} // namespace flub::nn
