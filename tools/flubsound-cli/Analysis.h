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

#include <array>
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

/** One octave band of octaveBands(). */
struct BandLevel
{
    float centreHz = 0.0f;
    float levelDb = -160.0f; // dBFS RMS (plain mean square) of the band
};

/** Octave-band levels (`--bands`) of the mean of all channels ((L + R) / 2
    for stereo), whole file: RBJ band-passes (constant 0 dB peak, Q sqrt 2,
    about one octave wide) at 31.5 Hz .. 16 kHz, each band only while its
    centre is below 0.4 x the sample rate. For comparing renders with each
    other (tonal balance, tools/scripts/preset-render-diff.py), not a
    class-1 IEC 61260 filter bank. Non-RT, allocates. */
std::vector<BandLevel> octaveBands (const std::vector<std::vector<float>>& channels, double sampleRate);

/** [{ "hz": 31.5, "db": -40.12 }, ...] (null for silence). */
json::Value bandsToJson (const std::vector<BandLevel>& bands);

/** "31.5 -40.1  63 -38.0 ... 16k -71.2 (octave band Hz: dBFS)" (one line, no newline). */
std::string formatBands (const std::vector<BandLevel>& bands);

/** Human-readable multi-line report. */
std::string formatReport (const LoudnessReport& report, const std::string& title, const std::string& sourceFormat);

/** JSON object with the same values (null for "no measurement"). */
json::Value reportToJson (const LoudnessReport& report, const std::string& file, const std::string& sourceFormat);

// ---- Sound-quality metrics (docs/11 E59) ---------------------------------
// Signal-level definitions shared by `flubsound-cli quality` (Commands.h,
// measureQuality) and tests/test_known_gaps.cpp, so a number in a tuning
// session and a number in a test mean the same thing. All are non-RT and
// operate on one channel (usually the mid); a "window" must hold an integer
// number of periods of every frequency it projects onto (1 s windows and
// integer-Hz tones make every projection exact).

/** THD+N of a sine at f0 over x[0, n): the power left after removing DC and
    the fundamental, dB re the window's total power. */
double sineThdnDb (const float* x, int n, double sampleRate, double f0);

/** Total distortion + noise of a multitone over x[0, n): the power left after
    removing DC and every excited tone, dB re the power of the excited tones
    (MTND when `tones` is a sparse multitone). */
double multitoneResidualDb (const float* x, int n, double sampleRate, const std::vector<double>& tones);

/** Intermodulation of two tones f1 < f2 over x[0, n): the power at every
    product |m f1 +- k f2| (m, k >= 1, m + k <= maxOrder) that is not a
    harmonic of either tone, dB re the power of the two tones. */
double twoToneImdDb (const float* x, int n, double sampleRate, double f1, double f2, int maxOrder = 5);

/** SMPTE-style IMD of a low tone fLow and a high tone fHigh over x[0, n): the
    power of the sidebands fHigh +- k fLow (k = 1..sidebands), dB re the high tone. */
double smpteImdDb (const float* x, int n, double sampleRate, double fLow, double fHigh, int sidebands = 4);

/** Envelope of a steady tone through a system: output / input amplitude at
    `freqHz` in Hann-weighted 20 ms windows every 5 ms over [begin, end), dB. */
std::vector<double> toneGainTrack (const std::vector<float>& out, const std::vector<float>& in, double sampleRate, double freqHz,
                                   int begin, int end);

/** Nearest-rank percentile (p in 0..1) of a non-empty vector. */
double percentile (std::vector<double> values, double p);

/** Summary of a toneGainTrack: spread p95 - p5, dip = median - min, lift =
    max - median, the share of windows more than 1 dB below the median, and
    the modulation spectrum: the amplitude (dB of gain) of the track's
    components at k x `rateHz`, k = 1..4, over the whole periods it holds. */
struct GainTrackStats
{
    double spreadDb = 0.0, dipDb = 0.0, liftDb = 0.0, downPercent = 0.0;
    std::array<double, 4> modulationDb {};
};

GainTrackStats summariseGainTrack (const std::vector<double>& track, double rateHz);

/** Energy centroid (ms) of x[begin, begin + n) re `begin`. */
double energyCentroidMs (const std::vector<float>& x, int begin, int n, double sampleRate);

// ---- Signal hygiene (docs/11 E10) ------------------------------------------
// FFT definitions (4-term Blackman-Harris window, sidelobes -92 dB), so the
// floor of every reading is about -100 dB.

/** The FFT bin (odd, so no fold of a harmonic lands on another harmonic) of
    a test tone near freqHz for an n-point analysis: the tone is then exactly
    bin * sampleRate / n. */
int aliasToneBin (double freqHz, double sampleRate, int n) noexcept;

/** Worst alias of a sine on FFT bin bin0 over x[0, n) (n a power of two):
    the largest line in [20 Hz, bandHz] that is not within 4 bins of a
    harmonic of the sine, dB re the fundamental (dBc). Every harmonic folds
    back at a bin of its own (bin0 odd), so what is left is aliasing and
    noise. */
double worstAliasDbc (const float* x, int n, double sampleRate, int bin0, double bandHz = 20000.0);

/** Share of x's power at and above fromHz (Welch, windowSize-point windows,
    50 % overlap), dB re the total (DC excluded). */
double powerShareAboveDb (const float* x, int n, double sampleRate, double fromHz, int windowSize = 8192);

/** |mean| of x[0, n) in dBFS (-160 floor). */
double dcDbfs (const float* x, int n);

/** "pcm16", "pcm24", "pcm32", "float32", "float64". */
const char* sampleFormatName (io::SampleFormat format) noexcept;

/** Formats a dB / LUFS value with the given decimals, or "-inf" at the floor. */
std::string formatDb (float value, int decimals = 1);
} // namespace flub::cli
