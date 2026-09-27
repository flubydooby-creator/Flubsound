// Flubsound Pro - the interface between AsyncModelProcessor and a model.
//
// A ModelRunner wraps one inference model (ONNX Runtime session, hand-written
// network, test stand-in). It never runs on the audio thread: the
// AsyncModelProcessor that owns it calls prepare() on the thread that prepares
// the processor (before its worker starts) and reset() / run() on its single
// inference worker thread, one frame at a time (in offline mode, which has
// no worker, on the thread that calls process()). So a runner needs no locking,
// may allocate in prepare() and should not allocate in run() (that only costs
// time, but time is what the deadline is made of).
//
// The model consumes fixed frames of audio and produces a *control frame*
// (docs/09 §1.1: prefer control signals over raw audio): numControls floats
// per model frame that the existing DSP applies, so artefacts stay bounded
// and the chain's limiter still guarantees the ceiling. Today the controls
// are linear gains (see ControlKind); per-band gains need a band-split
// renderer and are the next kind to add.
//
// No ONNX Runtime and no trained model ship yet. flub/neural/ReferenceRunners.h
// has stand-ins (identity, constant gain, always failing) for tests and hosts.
#pragma once

namespace flub
{
/** How AsyncModelProcessor applies a control frame. Every control is a linear
    gain; the processor clamps it to [0, AsyncModelConfig::maxGain] and smooths
    it, so a model can never produce a click or a boost past that bound. */
enum class ControlKind : int
{
    BroadbandGain = 0, // control[0] scales every channel (numControls >= 1; the rest are ignored)
    ChannelGains = 1   // control[c] scales output channel c; channels beyond numControls use the last one
};

/** Upper bounds a description must respect (AsyncModelProcessor stays inert otherwise). */
inline constexpr int kMaxModelFrameSize = 16384;
inline constexpr int kMaxModelControls = 256;

/** What a model consumes and produces. Constant for the runner's lifetime. */
struct ModelDescription
{
    /** Samples per channel in one model frame (1 .. kMaxModelFrameSize). */
    int frameSize = 0;

    /** Input channels: 1 = mono downmix (mean of the processed channels);
        N > 1 = planar channels 0 .. N-1, model channel c reading processed
        channel min(c, channels - 1). 1 .. kMaxChannels. */
    int numInputChannels = 1;

    /** Floats in one control frame (the model's output), 1 .. kMaxModelControls. */
    int numControls = 1;

    ControlKind controlKind = ControlKind::BroadbandGain;

    /** The rate the model was trained for; 0 = any. At a different processing
        rate the processor keeps its latency but does not run the model. */
    double sampleRate = 0.0;
};

class ModelRunner
{
public:
    virtual ~ModelRunner() = default;

    virtual ModelDescription describe() const = 0;

    /** Called by AsyncModelProcessor::prepare() before its worker starts (not
        on the audio thread). May allocate (sessions, scratch tensors). */
    virtual void prepare (double sampleRate) { (void) sampleRate; }

    /** Worker thread (offline mode: the caller of process()): clear
        recurrent state before the first frame after the processor was
        prepared or reset (the audio is discontinuous there). */
    virtual void reset() {}

    /** Worker thread (offline mode: the caller of process()). inFrame
        holds numInputChannels planar blocks of frameSize samples (channel c
        at inFrame[c * frameSize]); write numControls values to outControls. Return false on failure: the
        processor then treats the frame like a deadline miss (last good control,
        then neutral) and counts it in getModelFailures(). An exception
        escaping run() counts as a failure too (one escaping reset() is
        ignored). Must return in bounded time: a slow frame is only a deadline
        miss, but the processor's destructor and prepare() wait for the frame
        in flight. */
    virtual bool run (const float* inFrame, float* outControls) = 0;
};
} // namespace flub
