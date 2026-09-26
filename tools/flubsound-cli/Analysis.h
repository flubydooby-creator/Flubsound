// Flubsound Pro CLI - offline loudness / level analysis of a whole programme.
//
// Everything is measured with the same core meters the engine uses:
//   * flub::LoudnessMeter  (ITU-R BS.1770-4 / EBU R128): integrated loudness,
//     loudness range (EBU Tech 3342), maximum momentary / short-term loudness.
//     5.1 / 7.1 files are channel-weighted (LFE excluded, surrounds +1.5 dB).
//   * flub::TruePeakMeter  (4x oversampled, BS.1770 Annex 2): true peak.
//     The interpolator is flushed with silence at the end so inter-sample
//     peaks of the very last samples are not missed.
//   * sample peak and whole-file RMS (plain mean square, no +3 dB "AES"
//     sine convention: a full-scale sine reads -3.01 dBFS RMS).
// "No measurement" (silence, programme shorter than one 400 ms gating block)
// is reported as kMinusInfDb and printed as "-inf" (null in JSON).
#pragma once

#include "flub/io/Json.h"
#include "flub/io/WavFile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flub::cli
{
struct LoudnessReport
{
    double sampleRate = 0.0;
    int numChannels = 0;
    int64_t numFrames = 0;
    double durationSeconds = 0.0;

    float integratedLufs = -160.0f;
    float loudnessRangeLu = 0.0f;
    float maxMomentaryLufs = -160.0f;
    float maxShortTermLufs = -160.0f;

    float truePeakDbtp = -160.0f;  // max over all channels
    float samplePeakDbfs = -160.0f; // max over all channels
    float rmsDbfs = -160.0f;        // over all channels and samples

    std::vector<float> channelTruePeakDbtp, channelSamplePeakDbfs, channelRmsDbfs;
};

/** Measures planar audio (channels.size() in 1..8, all channels equally long).
    Non-RT, allocates. */
LoudnessReport analyse (const std::vector<std::vector<float>>& channels, double sampleRate);

/** Human-readable multi-line report. */
std::string formatReport (const LoudnessReport& report, const std::string& title, const std::string& sourceFormat);

/** JSON object with the same values (null for "no measurement"). */
json::Value reportToJson (const LoudnessReport& report, const std::string& file, const std::string& sourceFormat);

/** "pcm16", "pcm24", "pcm32", "float32", "float64". */
const char* sampleFormatName (io::SampleFormat format) noexcept;

/** Formats a dB / LUFS value with the given decimals, or "-inf" at the floor. */
std::string formatDb (float value, int decimals = 1);
} // namespace flub::cli
