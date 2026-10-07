// Flubsound Pro - which neural models a latency profile may run
// (docs/09-future-roadmap.md §1.1, "Latency profiles decide eligibility").
//
// The unit is the reference model frame, kNeuralReferenceFrameMs (10 ms, the
// hop of the voice-denoise class of models). A model's latency is what its
// processor reports (AsyncModelProcessor: frameSize * (1 + safetyFrames), or
// frameSize * (2 + safetyFrames) for ControlKind::BandGains):
//   Offline (batch render) : any model. No real-time deadline: the chain
//                            runs the processor in offline mode (the model
//                            runs inside process(), so a render faster than
//                            real time still gets every frame's result).
//   Quality                : any model ("heavy" models are allowed only here
//                            and offline).
//   Balanced               : light models, latency <= 2 reference frames
//                            (one 10 ms frame plus one frame of safety).
//   Low Latency            : latency <= 1 reference frame.
// Negative latencies and non-positive / non-finite sample rates are never
// eligible. ProcessingChain enforces it at prepare(): an ineligible model
// stays out of the chain (no latency added) and getNeuralStatus() says so.
#pragma once

#include "flub/engine/Parameters.h"

namespace flub
{
inline constexpr double kNeuralReferenceFrameMs = 10.0;

enum class ModelContext : int
{
    Realtime = 0, // live playback through the app / plug-in
    Offline = 1   // batch rendering (CLI, offline export): AsyncModelConfig::offline, never on an audio thread
};

/** Latency budget of a profile in reference frames (Quality: unbounded, returns 0). */
int neuralLatencyBudgetFrames (param::LatencyProfileValue profile) noexcept;

bool isEligible (param::LatencyProfileValue profile, int modelLatencySamples, double sampleRate,
                 ModelContext context = ModelContext::Realtime) noexcept;
} // namespace flub
