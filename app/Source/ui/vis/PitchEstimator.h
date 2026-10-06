// Flubsound Pro - multi-pitch estimate for the music views (docs/06 §6.4.2).
//
// What it does: from the post tap's mid signal it estimates which notes are
// sounding (their fundamentals, not their overtones) and a 12-bin chroma (the
// spectrum folded onto the pitch classes C .. B). The "fundamentals only"
// piano keys, the chord name, the chromagram and the key views all use it.
//
// Analysis (message thread only, like every view; no allocation after
// setSampleRate):
//   * Decimation by round (fs / 16 kHz) (3 at 44.1 / 48 kHz) behind an
//     8th-order Butterworth low-pass at 0.3 x the decimated rate.
//   * Every kHop decimated samples (32 ms at 16 kHz) a Blackman-windowed
//     kWindow-sample frame (0.51 s at 16 kHz: the Sharper lows' resolution,
//     1.95 Hz bins, so bass notes a semitone apart are told apart from about
//     G2 up, and a bass note by its harmonics below that), zero-padded to a
//     kFftSize-point FFT.
//   * findPeaks: local maxima above kFloorDb dBFS and within kRangeDb of the
//     loudest, frequency and level by parabolic interpolation in dB (a sine of
//     amplitude a reads a), between kMinHz and getMaxHz() (0.28 x the
//     decimated rate: 4.48 kHz).
//   * estimateNotes: harmonic summation over the note grid C1 .. C8 with
//     iterative subtraction. Every candidate note needs a peak at its
//     fundamental (within kToleranceCents; at least kMinFundamental on the
//     compressed scale, so a missing fundamental never makes a note an
//     octave down, and at least kMinFundamentalShare of it not explained by
//     the notes found so far); its salience is the sum over its first
//     kMaxHarmonics harmonics of w(h) = h^-0.5 times the matched peak's
//     level on a compressed (dB) scale times the share of the peak still
//     unexplained. The best note is taken and its harmonics are
//     subtracted from the peaks (by the spectral-smoothness estimate:
//     at most the mean of the harmonic and its neighbours, so a partial that
//     two notes share keeps the other note's part); repeat until kMaxNotes or
//     the best salience falls under kMinSalience or kRelativeSalience x the
//     first note's. At most one note is taken below kBassSplitMidi (C3): a
//     bass line plays one note at a time, and a kick drum's low smear would
//     otherwise read as a cluster of low notes.
//   * foldChroma: every peak from kChromaMinHz up adds its level on a
//     kChromaRangeDb scale, squared (x = 0 at kChromaRangeDb under the
//     loudest peak .. 1 there, weight x^2: the notes stand out of the noise
//     floor), to its pitch class (split between the two nearest by its offset
//     in semitones), normalised so the largest bin is 1. Below 100 Hz there is
//     mostly the kick drum; a bass note there counts by its harmonics.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace flub::app::ui::vis
{
struct SpectralPeak
{
    float hz = 0.0f;
    float amplitude = 0.0f; // linear: a sine of amplitude a reads a
};

struct EstimatedNote
{
    int midi = 0;          // 24 (C1) .. 108 (C8)
    float salience = 0.0f; // harmonic sum (compressed scale)
    float level = 0.0f;    // the fundamental's level on the compressed scale (0..1, 1 = loudest peak)
};

class PitchEstimator
{
public:
    static constexpr int kLowMidi = 24, kHighMidi = 108; // C1 .. C8
    static constexpr int kNumCandidates = kHighMidi - kLowMidi + 1;
    static constexpr int kMaxNotes = 6;
    static constexpr int kMaxPeaks = 128;
    static constexpr int kMaxHarmonics = 10;

    static constexpr double kTargetRate = 16000.0;
    static constexpr int kWindow = 8192;
    static constexpr int kFftOrder = 14;
    static constexpr int kFftSize = 1 << kFftOrder; // zero padding x2
    static constexpr int kHop = 512;
    static constexpr double kMinHz = 27.0;
    static constexpr float kFloorDb = -84.0f;
    static constexpr float kRangeDb = 50.0f;
    static constexpr float kToleranceCents = 35.0f;
    static constexpr float kMinFundamental = 0.3f;
    static constexpr float kMinFundamentalShare = 0.2f;
    static constexpr float kMinSalience = 0.45f;
    static constexpr float kRelativeSalience = 0.3f;
    static constexpr float kChromaMinHz = 100.0f;
    static constexpr float kChromaRangeDb = 36.0f;
    static constexpr int kBassSplitMidi = 48; // C3: at most one note below (a bass line plays one note)
    static constexpr double kIdleSeconds = 0.35;

    PitchEstimator();

    void setSampleRate (double sampleRate);
    double getDecimatedRate() const noexcept { return decimatedRate; }
    double getMaxHz() const noexcept { return maxHz; }
    /** Forgets the history and the result. */
    void reset() noexcept;
    /** New mono samples at the sample rate. */
    void push (const float* samples, int numSamples) noexcept;
    /** Runs the latest pending analysis (at most one per call); true when the
        result changed (a new analysis, or the result cleared after
        kIdleSeconds without samples). */
    bool update (double dtSeconds) noexcept;

    /** The estimated notes, lowest first. */
    int getNumNotes() const noexcept { return numNotes; }
    const EstimatedNote& getNote (int i) const noexcept { return notes[static_cast<size_t> (i)]; }
    /** The latest chroma (C .. B, largest 1; all 0 without signal). */
    const std::array<float, 12>& getChroma() const noexcept { return chroma; }
    bool hasSignal() const noexcept { return signal; }
    int getNumPeaks() const noexcept { return numPeaks; }
    const SpectralPeak* getPeaks() const noexcept { return peaks.data(); }

    // ---- Pure helpers (tested) ---------------------------------------------------------
    static double midiToHz (double midi) noexcept;
    /** Peaks of a magnitude spectrum (bin k at k x binHz; amplitude = magnitude
        x amplitudeScale), ascending in frequency; returns how many. */
    static int findPeaks (const float* magnitude, int numBins, double binHz, float scale, double minFrequency, double maxFrequency,
                          SpectralPeak* out, int maxPeaks) noexcept;
    /** Fundamentals from peaks sorted by frequency (see above); out lowest first. */
    static int estimateNotes (const SpectralPeak* peakList, int peakCount, double maxFrequency, EstimatedNote* out, int maxNotes) noexcept;
    /** Chroma (12 values) from peaks; false (all 0) when there are none. */
    static bool foldChroma (const SpectralPeak* peakList, int peakCount, float* out) noexcept;

private:
    void analyse() noexcept;
    void clearResult() noexcept;

    juce::dsp::FFT fft { kFftOrder };
    std::vector<float> window, history, fftData;
    float amplitudeScale = 1.0f;
    double sampleRate = 48000.0, decimatedRate = 16000.0, maxHz = 4480.0, idleSeconds = 0.0;
    int decimation = 3, phase = 0, writePos = 0, filled = 0, sinceHop = 0;
    std::array<double, 20> coeffs {}; // 4 biquads: b0 b1 b2 a1 a2
    std::array<double, 8> state {};
    std::array<SpectralPeak, kMaxPeaks> peaks {};
    int numPeaks = 0;
    std::array<EstimatedNote, kMaxNotes> notes {};
    int numNotes = 0;
    std::array<float, 12> chroma {};
    bool signal = false, pending = false;
};
} // namespace flub::app::ui::vis
