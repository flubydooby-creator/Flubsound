// Flubsound Pro CLI - sample-aligned offline rendering through ProcessingChain.
//
// Every pass builds its own ParameterStore + ProcessingChain (so batch jobs
// on worker threads share nothing) and runs exactly the real-time code path:
//
//   * ScopedNoDenormals for the whole pass.
//   * Channels: mono is duplicated to stereo; stereo as is; 6 / 8 channels
//     (5.1 / 7.1, WAVE_FORMAT_EXTENSIBLE order FL FR FC LFE [BL BR] SL SR) run
//     through a chain prepared with inputChannels 6 / 8, which virtualises
//     them binaurally (virt.on) or downmixes them (ITU-R BS.775). The output
//     is always stereo.
//   * Parameter priming: after prepare() one block of silence is processed
//     (the chain pushes every parameter into its modules) and the chain is
//     reset(), which snaps the modules' smoothers onto those targets - so the
//     render starts at the requested settings instead of gliding into them.
//   * Latency compensation: the chain delays by L = getLatencySamples(). The
//     input is followed by L samples of silence and the first L output
//     samples are discarded, so the output has exactly the input length and
//     alignment (a bypassed render is the input, sample for sample).
//
// Loudness targeting (targetLufs): render, measure integrated loudness with
// flub::LoudnessMeter, move the maximizer drive (max.drive, 0..24 dB) by the
// error - from the second correction on, divided by the measured
// loudness-per-dB slope (secant step), because a limiter's response flattens
// as it works harder - and re-render, up to maxIterations times, stopping
// within toleranceLu. When even 0 dB drive is too loud, the remaining excess
// is removed with output.gain (post-maximizer attenuation, so the ceiling
// still holds). The pass closest to the target is delivered. The true-peak
// ceiling itself is enforced by the maximizer's true-peak limiter.
#pragma once

#include "Analysis.h"

#include "flub/io/WavFile.h"

#include <optional>
#include <string>
#include <vector>

namespace flub::cli
{
struct RenderSettings
{
    int blockSize = 512;
    std::optional<float> targetLufs;
    float toleranceLu = 0.3f;
    int maxIterations = 4;                 // corrective re-renders after the first pass
    std::optional<float> verifyCeilingDb;  // report a true peak above this (+0.1 dB)
};

struct RenderResult
{
    io::AudioFileData output;     // 2 channels, same rate and length as the input
    LoudnessReport outputReport;  // analysis of `output`
    int latencySamples = 0;       // compensated chain latency
    int chainInputChannels = 2;
    int passes = 0;
    float driveDb = 0.0f;         // base max.drive of the delivered pass
    float outputGainDb = 0.0f;    // base output.gain of the delivered pass
    bool targetReached = true;
    double renderSeconds = 0.0;   // wall time of all passes (excluding analysis)
    std::vector<std::string> notes; // problems start with "warning: "
};

/** Checks sample rate / channel count / length before any work starts. */
bool checkRenderable (const io::AudioFileData& input, std::string& error);

/** Renders `input` with the base parameter table (param::kNumParams values),
    including the optional loudness-targeting loop. Non-RT, allocates. */
bool renderFile (const io::AudioFileData& input, const std::vector<float>& baseValues, const RenderSettings& settings,
                 RenderResult& result, std::string& error);

/** A single latency-compensated pass (no targeting). `outStereo` receives
    2 planar channels of input length. */
bool renderPass (const io::AudioFileData& input, const std::vector<float>& values, int blockSize,
                 std::vector<std::vector<float>>& outStereo, int& latencySamples, std::string& error);
} // namespace flub::cli
