#include "flub/neural/TinyNet.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub::nn
{
namespace
{
constexpr std::array<uint32_t, 256> makeCrcTable() noexcept
{
    std::array<uint32_t, 256> table {};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = makeCrcTable();

/** Bounds-checked little-endian reader over the payload. */
class Reader
{
public:
    Reader (const uint8_t* bytes, size_t length) noexcept : data (bytes), size (length) {}

    size_t remaining() const noexcept { return size - pos; }

    bool u32 (uint32_t& v) noexcept
    {
        if (remaining() < 4)
            return false;
        v = static_cast<uint32_t> (data[pos]) | (static_cast<uint32_t> (data[pos + 1]) << 8)
          | (static_cast<uint32_t> (data[pos + 2]) << 16) | (static_cast<uint32_t> (data[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    /** count float32 values appended to dst; false if short or not finite. */
    bool f32 (std::vector<float>& dst, size_t count)
    {
        if (count > remaining() / 4)
            return false;
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t bits = 0;
            u32 (bits);
            float v = 0.0f;
            std::memcpy (&v, &bits, sizeof v);
            if (! std::isfinite (v))
                return false;
            dst.push_back (v);
        }
        return true;
    }

    /** rows int8 rows of cols values with per-row scales, dequantised into dst. */
    bool int8Rows (std::vector<float>& dst, size_t rows, size_t cols)
    {
        std::vector<float> scales;
        if (! f32 (scales, rows))
            return false;
        const size_t count = rows * cols;
        const size_t padded = (count + 3) / 4 * 4;
        if (padded > remaining())
            return false;
        for (size_t r = 0; r < rows; ++r)
        {
            if (scales[r] < 0.0f)
                return false;
            for (size_t c = 0; c < cols; ++c)
            {
                const auto q = static_cast<int8_t> (data[pos + r * cols + c]);
                if (q == -128)
                    return false; // the writer never emits -128 (symmetric range)
                dst.push_back (static_cast<float> (q) * scales[r]);
            }
        }
        for (size_t i = count; i < padded; ++i)
            if (data[pos + i] != 0)
                return false;
        pos += padded;
        return true;
    }

    bool bytes (std::string& dst, size_t count)
    {
        const size_t padded = (count + 3) / 4 * 4;
        if (padded > remaining())
            return false;
        dst.assign (reinterpret_cast<const char*> (data + pos), count);
        for (size_t i = count; i < padded; ++i)
            if (data[pos + i] != 0)
                return false;
        pos += padded;
        return true;
    }

private:
    const uint8_t* data;
    size_t size, pos = 0;
};

/** Four independent partial sums in a fixed order: vectorisable, and the same
    bits on every run. */
inline float dot (const float* a, const float* b, int n) noexcept
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; ++i)
        s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

inline float sigmoid (float v) noexcept
{
    return 1.0f / (1.0f + std::exp (-v));
}

inline float activate (Activation a, float v) noexcept
{
    switch (a)
    {
        case Activation::Relu: return v > 0.0f ? v : 0.0f;
        case Activation::Tanh: return std::tanh (v);
        case Activation::Sigmoid: return sigmoid (v);
        case Activation::Linear: break;
    }
    return v;
}

std::string str (uint64_t v)
{
    return std::to_string (v);
}
} // namespace

uint32_t crc32 (const void* data, size_t size) noexcept
{
    const auto* p = static_cast<const uint8_t*> (data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = kCrcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool TinyNet::load (const void* raw, size_t size, std::string& error)
{
    *this = TinyNet {};
    const auto fail = [&error] (std::string why)
    {
        error = std::move (why);
        return false;
    };
    const auto* bytes = static_cast<const uint8_t*> (raw);
    if (bytes == nullptr || size < kHeaderBytes)
        return fail ("the data is shorter than the 64-byte model header");
    if (std::memcmp (bytes, "FLUBTNET", 8) != 0)
        return fail ("not a Flubsound TinyNet model (the file does not start with FLUBTNET)");

    Reader h (bytes + 8, kHeaderBytes - 8);
    uint32_t formatVersion = 0, headerBytes = 0, payloadBytes = 0, crc = 0, modelVersion = 0, featureSet = 0;
    uint32_t sampleRate = 0, frameSize = 0, numFeatures = 0, numLayers = 0, numOutputs = 0, parameterCount = 0, flags = 0, reserved = 0;
    for (uint32_t* v : { &formatVersion, &headerBytes, &payloadBytes, &crc, &modelVersion, &featureSet, &sampleRate, &frameSize,
                         &numFeatures, &numLayers, &numOutputs, &parameterCount, &flags, &reserved })
        h.u32 (*v);
    if (formatVersion != kFormatVersion)
        return fail ("unsupported model format version " + str (formatVersion) + " (this build reads version " + str (kFormatVersion) + ")");
    if (headerBytes != kHeaderBytes)
        return fail ("unexpected header size " + str (headerBytes));
    if (static_cast<uint64_t> (payloadBytes) != static_cast<uint64_t> (size - kHeaderBytes))
        return fail ("the header announces " + str (payloadBytes) + " payload bytes but the data has " + str (size - kHeaderBytes)
                     + " (truncated or padded file)");
    if (crc32 (bytes + kHeaderBytes, payloadBytes) != crc)
        return fail ("checksum mismatch: the model data is corrupt");
    if (flags != 0 || reserved != 0)
        return fail ("unknown header flags");
    if (numFeatures < 1 || numFeatures > static_cast<uint32_t> (kMaxFeatures))
        return fail ("invalid input size " + str (numFeatures));
    if (numLayers < 1 || numLayers > static_cast<uint32_t> (kMaxLayers))
        return fail ("invalid layer count " + str (numLayers));
    if (numOutputs < 1 || numOutputs > static_cast<uint32_t> (kMaxOutputs))
        return fail ("invalid output count " + str (numOutputs));
    if (parameterCount > kMaxParameters)
        return fail ("too many parameters (" + str (parameterCount) + ")");

    Reader r (bytes + kHeaderBytes, payloadBytes);
    std::vector<int> sizes { static_cast<int> (numFeatures) };
    std::vector<Layer> parsed;
    std::vector<float> w;
    w.reserve (std::min<size_t> (parameterCount, payloadBytes));
    size_t stateFloats = 0, gatherFloats = 0, gateFloats = 0;
    uint64_t params = 0;
    for (uint32_t l = 0; l < numLayers; ++l)
    {
        const std::string where = "layer " + str (l) + ": ";
        uint32_t type = 0, act = 0, out = 0, kernel = 0, nIn = 0, format = 0;
        if (! r.u32 (type) || ! r.u32 (act) || ! r.u32 (out) || ! r.u32 (kernel) || ! r.u32 (nIn))
            return fail (where + "truncated");
        if (type < 1 || type > 3)
            return fail (where + "unknown layer type " + str (type));
        if (act > 3)
            return fail (where + "unknown activation " + str (act));
        Layer layer;
        layer.type = static_cast<LayerType> (type);
        layer.act = static_cast<Activation> (act);
        if (layer.type == LayerType::Gru && layer.act != Activation::Linear)
            return fail (where + "a GRU has fixed activations (the field must be 0)");
        if (out < 1 || out > static_cast<uint32_t> (kMaxUnits))
            return fail (where + "invalid output size " + str (out));
        const bool conv = layer.type == LayerType::Conv1D;
        if ((conv && (kernel < 1 || kernel > static_cast<uint32_t> (kMaxKernel))) || (! conv && kernel != 1))
            return fail (where + "invalid kernel " + str (kernel));
        if (nIn < 1 || nIn > static_cast<uint32_t> (kMaxLayerInputs))
            return fail (where + "invalid input count " + str (nIn));
        layer.out = static_cast<int> (out);
        layer.kernel = static_cast<int> (kernel);
        layer.numInputs = static_cast<int> (nIn);
        for (uint32_t i = 0; i < nIn; ++i)
        {
            uint32_t id = 0;
            if (! r.u32 (id))
                return fail (where + "truncated");
            if (id >= sizes.size())
                return fail (where + "input tensor " + str (id) + " is not an earlier tensor");
            layer.inputs[i] = static_cast<int> (id);
            layer.in += sizes[id];
        }
        if (! r.u32 (format) || format > 1)
            return fail (where + "unknown weight format");

        const auto rows = static_cast<size_t> (layer.type == LayerType::Gru ? 3 * layer.out : layer.out);
        const auto cols = static_cast<size_t> (conv ? layer.kernel * layer.in : layer.in);
        const auto readMatrix = [&] (size_t nr, size_t nc)
        {
            return format == 1 ? r.int8Rows (w, nr, nc) : r.f32 (w, nr * nc);
        };
        const bool gru = layer.type == LayerType::Gru;
        const uint64_t layerParams = rows * cols + (gru ? rows * static_cast<size_t> (layer.out) + 2 * rows : rows);
        if (params + layerParams > kMaxParameters)
            return fail (where + "too many parameters");
        layer.w = w.size();
        if (! readMatrix (rows, cols))
            return fail (where + "weights truncated or not finite");
        params += rows * cols;
        if (layer.type == LayerType::Gru)
        {
            layer.u = w.size();
            if (! readMatrix (rows, static_cast<size_t> (layer.out)))
                return fail (where + "recurrent weights truncated or not finite");
            params += rows * static_cast<size_t> (layer.out);
        }
        layer.b = w.size();
        if (! r.f32 (w, rows))
            return fail (where + "biases truncated or not finite");
        params += rows;
        if (layer.type == LayerType::Gru)
        {
            layer.bh = w.size();
            if (! r.f32 (w, rows))
                return fail (where + "recurrent biases truncated or not finite");
            params += rows;
            layer.state = stateFloats;
            stateFloats += static_cast<size_t> (layer.out);
            gateFloats = std::max (gateFloats, 6 * static_cast<size_t> (layer.out));
        }
        else if (conv)
        {
            layer.state = stateFloats;
            stateFloats += static_cast<size_t> (layer.kernel - 1) * static_cast<size_t> (layer.in);
        }
        if (layer.numInputs > 1)
            gatherFloats = std::max (gatherFloats, static_cast<size_t> (layer.in));
        sizes.push_back (layer.out);
        parsed.push_back (layer);
    }

    std::vector<int> outs;
    for (uint32_t i = 0; i < numOutputs; ++i)
    {
        uint32_t id = 0;
        if (! r.u32 (id))
            return fail ("output list truncated");
        if (id < 1 || id > numLayers)
            return fail ("output " + str (i) + " names tensor " + str (id) + ", which is not a layer's output");
        outs.push_back (static_cast<int> (id));
    }
    uint32_t nameBytes = 0;
    std::string name;
    if (! r.u32 (nameBytes) || nameBytes > kMaxNameBytes || ! r.bytes (name, nameBytes))
        return fail ("name missing or invalid");
    if (r.remaining() != 0)
        return fail (str (r.remaining()) + " unexpected bytes after the model");
    if (params != parameterCount)
        return fail ("the header announces " + str (parameterCount) + " parameters but the layers hold " + str (params));

    // Everything checks out: allocate the run-time state.
    layers = std::move (parsed);
    weights = std::move (w);
    tensorOffset.clear();
    tensorSize = sizes;
    size_t offset = 0;
    for (int s : sizes)
    {
        tensorOffset.push_back (offset);
        offset += static_cast<size_t> (s);
    }
    tensors.assign (offset, 0.0f);
    for (size_t i = 0; i < layers.size(); ++i)
        layers[i].outOffset = tensorOffset[i + 1];
    gateOffset = gatherFloats;
    scratch.assign (gatherFloats + gateFloats, 0.0f);
    state.assign (stateFloats, 0.0f);
    outputTensors = std::move (outs);
    modelInfo.formatVersion = formatVersion;
    modelInfo.modelVersion = modelVersion;
    modelInfo.featureSet = featureSet;
    modelInfo.sampleRate = sampleRate;
    modelInfo.frameSize = frameSize;
    modelInfo.numFeatures = numFeatures;
    modelInfo.numLayers = numLayers;
    modelInfo.numOutputs = numOutputs;
    modelInfo.parameterCount = parameterCount;
    modelInfo.name = std::move (name);
    loaded = true;
    error.clear();
    return true;
}

int TinyNet::outputSize (int index) const noexcept
{
    if (index < 0 || index >= numOutputs())
        return 0;
    return tensorSize[static_cast<size_t> (outputTensors[static_cast<size_t> (index)])];
}

const float* TinyNet::output (int index) const noexcept
{
    if (index < 0 || index >= numOutputs())
        return nullptr;
    return tensors.data() + tensorOffset[static_cast<size_t> (outputTensors[static_cast<size_t> (index)])];
}

void TinyNet::reset() noexcept FLUB_NONBLOCKING
{
    std::fill (state.begin(), state.end(), 0.0f);
}

const float* TinyNet::gatherInput (const Layer& layer) noexcept
{
    if (layer.numInputs == 1)
        return tensors.data() + tensorOffset[static_cast<size_t> (layer.inputs[0])];
    float* dst = scratch.data();
    for (int i = 0; i < layer.numInputs; ++i)
    {
        const auto t = static_cast<size_t> (layer.inputs[static_cast<size_t> (i)]);
        const float* src = tensors.data() + tensorOffset[t];
        dst = std::copy (src, src + tensorSize[t], dst);
    }
    return scratch.data();
}

void TinyNet::run (const float* input) noexcept FLUB_NONBLOCKING
{
    if (! loaded)
        return;
    std::copy (input, input + tensorSize[0], tensors.begin());
    for (const Layer& layer : layers)
    {
        const float* x = gatherInput (layer);
        float* y = tensors.data() + layer.outOffset;
        const float* W = weights.data() + layer.w;
        const float* B = weights.data() + layer.b;
        const int in = layer.in;
        switch (layer.type)
        {
            case LayerType::Dense:
                for (int o = 0; o < layer.out; ++o)
                    y[o] = activate (layer.act, B[o] + dot (W + static_cast<size_t> (o) * static_cast<size_t> (in), x, in));
                break;

            case LayerType::Conv1D:
            {
                float* hist = state.data() + layer.state; // (kernel - 1) frames, oldest first
                const int histLen = (layer.kernel - 1) * in;
                const int rowLen = layer.kernel * in;
                for (int o = 0; o < layer.out; ++o)
                {
                    const float* row = W + static_cast<size_t> (o) * static_cast<size_t> (rowLen);
                    y[o] = activate (layer.act, B[o] + dot (row, hist, histLen) + dot (row + histLen, x, in));
                }
                if (histLen > 0)
                {
                    std::copy (hist + in, hist + histLen, hist);
                    std::copy (x, x + in, hist + histLen - in);
                }
                break;
            }

            case LayerType::Gru:
            {
                const int hd = layer.out;
                float* h = state.data() + layer.state;
                float* gx = scratch.data() + gateOffset;
                float* gh = gx + 3 * hd;
                const float* U = weights.data() + layer.u;
                const float* Bh = weights.data() + layer.bh;
                for (int j = 0; j < 3 * hd; ++j)
                {
                    gx[j] = B[j] + dot (W + static_cast<size_t> (j) * static_cast<size_t> (in), x, in);
                    gh[j] = Bh[j] + dot (U + static_cast<size_t> (j) * static_cast<size_t> (hd), h, hd);
                }
                for (int i = 0; i < hd; ++i)
                {
                    const float z = sigmoid (gx[i] + gh[i]);
                    const float rr = sigmoid (gx[hd + i] + gh[hd + i]);
                    const float n = std::tanh (gx[2 * hd + i] + rr * gh[2 * hd + i]);
                    y[i] = (1.0f - z) * n + z * h[i];
                }
                std::copy (y, y + hd, h);
                break;
            }
        }
    }
}
} // namespace flub::nn
