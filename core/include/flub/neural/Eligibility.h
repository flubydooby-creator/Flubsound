// Flubsound Pro - which neural models a latency profile may run
// (docs/09-future-roadmap.md §1.1, "Latency profiles decide eligibility").
//
// The unit is the reference model frame, kNeuralReferenceFrameMs (10 ms, the
// hop of the voice-denoise class of models). A model's latency is what its
// processor reports (AsyncModelProcessor: frameSize * (1 + safetyFrames)):
//   Offline (batch render) : any model; nothing waits for the output.
//   Quality                : any model ("heavy" models are allowed only here
//                            and offline).
//   Balanced               : light models, latency <= 2 reference frames
//                            (one 10 ms frame plus one frame of safety).
//   Low Latency            : latency <= 1 reference frame.
// Negative latencies and non-positive / non-finite sample rates are never
// eligible. The rule only answers the question; nothing enforces it yet,
// since no neural module is in ProcessingChain.
#pragma once

#include "flub/engine/Parameters.h"

namespace flub
{
inline constexpr double kNeuralReferenceFrameMs = 10.0;

enum class ModelContext : int
{
    Realtime = 0, // live playback through the app / plug-in
    Offline = 1   // batch rendering (CLI, offline export)
};

/** Latency budget of a profile in reference frames (Quality: unbounded, returns 0). */
int neuralLatencyBudgetFrames (param::LatencyProfileValue profile) noexcept;

bool isEligible (param::LatencyProfileValue profile, int modelLatencySamples, double sampleRate,
                 ModelContext context = ModelContext::Realtime) noexcept;
} // namespace flub
