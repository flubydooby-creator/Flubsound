// Flubsound Pro CLI - sample-aligned offline rendering through ProcessingChain.
//
// Plain C++ on flub::core (with Analysis.cpp): the CLI, the core tests and
// the desktop app's Export / batch dialog (app/Source/export) compile it, so
// all three render a file identically.
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
// flub::LoudnessMeter, move one gain stage by the error and re-render, up to
// maxIterations times, stopping within toleranceLu. From the second move of
// the same stage on the step is divided by the measured loudness-per-dB slope
// (secant step), because a limiter's response flattens as it works harder.
// Stages, in order: louder = max.drive (0..24 dB), then input.gain (ahead of
// the maximizer, so the ceiling still holds); quieter = max.drive down to
// 0 dB, then output.gain (post-maximizer attenuation), then input.gain. With
// the maximizer off output.gain is used (and input.gain once output.gain is
// at the end of its range). The pass closest to the target is delivered.
// Targeting is skipped (with a warning) for bypass=on and for programmes
// without a measurable integrated loudness.
//
// Ceiling: the maximizer's true-peak limiter enforces it. If the delivered
// pass still measures above verifyCeilingDb (limiter overshoot at extreme
// drive) and the maximizer is on, a static trim brings the file back under
// the ceiling (ceilingTrimDb, reported as a note).
//
// Render statistics (RenderResult::stats, `render.stats` in --json): after
// every block that carries programme the pass reads the chain's MeterBus -
// the same per-block readings the app's meters show - and accumulates the
// deepest, mean and time-above values of limiter / glue / compressor gain
// reduction, clip energy, measured THD+N, bass protection, the dynamic EQ's
// mode bands, the SafetyGovernor's scale, state and reasons, AutoLevel /
// AutoDrive, and the intended harmonics of the bass harmonics generator and
// air exciter (MeterBus::harmonicsDb). Means are weighted by block length;
// "percent" values are shares of the programme's frames. The governor's
// scale, state and reason bits are also kept as read after the last block
// of programme (governorScaleEnd / StateEnd / ReasonEnd, docs/11 E06 (6)),
// and at protection strength Normal / Strict the measured loop's readouts:
// harmonics and tonal scales, the audible residuals it compares, the output
// PLR, the brightness lifts, with their budgets (docs/11 E06 batch 2).
#pragma once

#include "Analysis.h"

#include "flub/engine/Protection.h"
#include "flub/io/WavFile.h"

#include <array>
#include <atomic>
#include <cstdint>
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
    std::optional<float> verifyCeilingDb;  // hold (maximizer on) / report a true peak above this
    ProtectionStrength protection = ProtectionStrength::Off; // ProcessingChain::setProtectionStrength
    // Optional: another thread sets it to stop the render between blocks
    // (renderFile / renderPass then fail with kAbortedError). The CLI leaves
    // it null; the app's Export / batch job uses it to quit without waiting.
    const std::atomic<bool>* abort = nullptr;
};

/** The error of a render stopped through RenderSettings::abort. */
inline constexpr const char* kAbortedError = "render aborted";

/** Statistics of one pass, polled from the chain's MeterBus once per block
    (see the header comment). Gain reductions are dB <= 0; "max" is the
    deepest reading. A module that is off reads as "no action". */
struct RenderStats
{
    int64_t frames = 0; // programme frames covered (the latency tail is not counted)

    // Maximizer: true-peak limiter, multiband glue, soft clipper.
    float limiterGrMaxDb = 0.0f, limiterGrMeanDb = 0.0f;
    float limiterOver1DbPercent = 0.0f, limiterOver3DbPercent = 0.0f; // frames limited deeper than 1 / 3 dB
    float glueGrMaxDb = 0.0f, glueGrMeanDb = 0.0f;
    float clipEnergyMaxDb = -160.0f;  // clipped-off energy re the output, loudest block
    float clipActivePercent = 0.0f;   // frames whose clip energy is above -60 dB
    uint64_t safetyClips = 0;         // engagements of the limiter's final safety clamp

    // Measured THD+N of saturator + clipper (the 300 ms smoothed meter, dB).
    float distortionMaxDb = -160.0f, distortionMeanDb = -160.0f; // mean = power mean
    // Harmonics the bass harmonics generator and air exciter add on purpose
    // (dB re their output, 300 ms smoothing; not budgeted by the governor).
    float harmonicsMaxDb = -160.0f, harmonicsMeanDb = -160.0f; // mean = power mean

    // Compressor, bass protection, dynamic EQ mode bands 4..7 (Gaming:
    // footsteps / body / anti-masking / voice; Music: de-harsh / air / de-boom / 1 kHz).
    float compGrMaxDb = 0.0f, compGrMeanDb = 0.0f, compUpwardMaxDb = 0.0f;
    float bassProtectionMaxDb = 0.0f; // boost withdrawn (dB >= 0)
    std::array<float, 4> modeBandMinDb {}, modeBandMaxDb {}, modeBandMeanDb {};

    // Control loops.
    float governorScaleMin = 1.0f, governorScaleMean = 1.0f;
    float governorBackoffPercent = 0.0f; // frames with the Boost scale below 0.99
    // Frames per SafetyGovernor::State (idle, backing off, holding,
    // recovering) and frames whose reason bits name the limiter / distortion budget.
    std::array<float, 4> governorStatePercent {};
    float governorLimiterReasonPercent = 0.0f, governorDistortionReasonPercent = 0.0f;
    // After the last block of programme: scale, SafetyGovernor::State and reason bits.
    float governorScaleEnd = 1.0f;
    int governorStateEnd = 0;
    uint32_t governorReasonEnd = 0;
    float autoLevelMinDb = 0.0f, autoLevelMaxDb = 0.0f;
    float autoDriveMaxDb = 0.0f;         // deepest drive reduction

    // The governor's measured loop (protection strength Normal / Strict,
    // docs/11 E06 batch 2; MeterBus::governor*): at Off the harmonics and
    // tonal scales stay 1 and nothing below is measured (-160 dB / 1000).
    int governorStrength = 0;            // the ProtectionStrength of the pass
    float governorHarmonicsScaleMin = 1.0f, governorHarmonicsScaleEnd = 1.0f;
    float governorTonalScaleMin = 1.0f, governorTonalScaleEnd = 1.0f;
    // Frames whose reason bits name the dynamics (PLR), harmonics and tonal budgets.
    float governorDynamicsReasonPercent = 0.0f, governorHarmonicsReasonPercent = 0.0f, governorTonalReasonPercent = 0.0f;
    // Audible residuals (dB re the output): what the drive loop compares
    // (max, power mean over the frames measured, at the end), the harmonics
    // loop's (max, end), the bass engine's whole span (end); the budget.
    float governorDriveResidualMaxDb = -160.0f, governorDriveResidualMeanDb = -160.0f, governorDriveResidualEndDb = -160.0f;
    float governorHarmonicsResidualMaxDb = -160.0f, governorHarmonicsResidualEndDb = -160.0f, governorBassResidualEndDb = -160.0f;
    float governorResidualBudgetDb = -35.0f;
    // The output's PLR over ~3 s (lowest reading, at the end; 1000 = none) and its budget (0 = none).
    float governorPlrMinDb = 1000.0f, governorPlrEndDb = 1000.0f, governorPlrBudgetDb = 8.0f;
    // Brightness (docs/11 E07): presence / harsh / air lift over the
    // 200 Hz - 1 kHz lift (dB; -160 = none), highest and at the end, and the budgets.
    std::array<float, 3> tonalLiftMaxDb { -160.0f, -160.0f, -160.0f }, tonalLiftEndDb { -160.0f, -160.0f, -160.0f };
    std::array<float, 3> tonalBudgetDb { 3.0f, 3.0f, 4.0f };
    // The Smoothness stage (docs/11 E07; MeterBus::smoothnessCutDb): its
    // deepest cut of the 5 - 10 kHz band (dB <= 0), and the frames it cut
    // more than 0.5 dB.
    float smoothnessCutMaxDb = 0.0f, smoothnessActivePercent = 0.0f;
};

struct RenderResult
{
    io::AudioFileData output;     // 2 channels, same rate and length as the input
    LoudnessReport outputReport;  // analysis of `output`
    int latencySamples = 0;       // compensated chain latency
    int chainInputChannels = 2;
    int passes = 0;
    float driveDb = 0.0f;         // base max.drive of the delivered pass
    float inputGainDb = 0.0f;     // base input.gain of the delivered pass
    float outputGainDb = 0.0f;    // base output.gain of the delivered pass
    float ceilingTrimDb = 0.0f;   // static trim applied to hold the ceiling (<= 0)
    bool targetReached = true;
    double renderSeconds = 0.0;   // wall time of all passes (excluding analysis)
    RenderStats stats;            // of the delivered pass
    std::vector<std::string> notes; // problems start with "warning: "
};

/** Checks sample rate / channel count / length before any work starts. */
bool checkRenderable (const io::AudioFileData& input, std::string& error);

/** Renders `input` with the base parameter table (param::kNumParams values),
    including the optional loudness-targeting loop. Non-RT, allocates. */
bool renderFile (const io::AudioFileData& input, const std::vector<float>& baseValues, const RenderSettings& settings,
                 RenderResult& result, std::string& error);

/** Writes result.output to `path` in `format` (Float32, Pcm24 or Pcm16) and
    sets result.outputReport to the analysis of the samples actually written
    (PCM files are read back: the report includes quantisation and TPDF
    dither; float32 round-trips bit-exactly, so the render's own analysis is
    the file's). Returns false with a message on failure. */
bool writeRender (const std::string& path, io::SampleFormat format, RenderResult& result, std::string& error);

/** A single latency-compensated pass (no targeting). `outStereo` receives
    2 planar channels of input length. A non-null `abort` is polled once per
    block (see RenderSettings::abort); a non-null `stats` receives the pass's
    statistics; `protection` is the chain's protection strength. */
bool renderPass (const io::AudioFileData& input, const std::vector<float>& values, int blockSize,
                 std::vector<std::vector<float>>& outStereo, int& latencySamples, std::string& error,
                 const std::atomic<bool>* abort = nullptr, RenderStats* stats = nullptr,
                 ProtectionStrength protection = ProtectionStrength::Off);
} // namespace flub::cli
