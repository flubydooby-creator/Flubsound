// Flubsound Pro - objective spatial metrics of binaural responses (docs/11
// E60 stage 2) and the HRTF-rendered ILD method for the positional focus
// (docs/11 E24).
//
// Offline (allocates, never on the audio thread). The measuring stick for
// every virtualiser and imaging change: `flubsound-cli analyze --spatial`
// and `--focus-ild`, tests/test_spatial_metrics.cpp and test_focus_ild.cpp.
//
// Binaural impulse responses (left / right ear of one source):
//   onset   the first sample where either ear reaches -20 dB re the
//           response's peak (the direct sound's arrival, t = 0);
//   IACC    max over |lag| <= 1 ms of |sum l(t) r(t + lag)| /
//           sqrt(sum l^2 sum r^2) (ISO 3382-1 A.3), early over
//           [onset - 1 ms, onset + 80 ms), late over [onset + 80 ms, end),
//           broadband and per octave band (125 Hz .. 8 kHz, 4th-order
//           Butterworth band edges at fc / sqrt 2 and fc sqrt 2, causal).
//           NaN when a window holds less than -60 dB of the response's
//           energy (e.g. no reverberant tail);
//   ITD     the lag of that maximum (early window, broadband; + = the
//           right ear lags);
//   DRR     both ears' energy in [onset - 0.5 ms, onset + 2.5 ms) over the
//           energy after it, dB (+inf without a reverberant part);
//   levels  1/3-octave band levels (nominal centres, band edges fc 2^-+1/6):
//           the mean |H|^2 of the zero-padded FFT bins in the band, dB (a
//           unit impulse reads 0 dB in every band).
// Diffuse-field response of a set of responses (directions): the 1/3-octave
// power average over every response and both ears; its deviation is that
// curve less its own mean over the bands (so a flat diffuse field reads 0
// everywhere, whatever its level), with the range (max - min) and the rms.
//
// ILD per 1/3-octave band of a stereo signal (noise, programme):
// 10 log10 (P_left / P_right) from Welch spectra (8192-point Hann, 50 %
// overlap); the ILD deviation of a processed signal is its band ILD less the
// reference's (e.g. the unprocessed HRTF-rendered source).
//
// HRTF-rendered sources (E24): a mono signal convolved with the parametric
// renderer's own HRIRs (HeadphoneVirtualizer::parametricHrir) at any
// azimuth; the positional focus (StereoSpatializer, width 1, space and
// crossfeed 0) applied to them at focus 0 / 50 / 100 %.
#pragma once

#include "flub/dsp/HeadphoneVirtualizer.h"

#include <array>
#include <cstdint>
#include <vector>

namespace flub
{
/** Left / right ear impulse responses of one source (equal lengths). */
struct BinauralIr
{
    std::vector<float> left, right;
};

/** Octave bands of the IACC (Hz). */
inline constexpr std::array<double, 7> kIaccOctavesHz { 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 };

/** Nominal 1/3-octave centres (Hz) from loHz to hiHz whose upper edge is
    below 0.5 fs (exact centres 1 kHz x 2^(k/3); these are their labels). */
std::vector<double> thirdOctaveBands (double sampleRate, double loHz = 50.0, double hiHz = 20000.0);

/** Exact centre (1 kHz x 2^(k/3)) of a nominal 1/3-octave label. */
double thirdOctaveExactHz (double nominalHz) noexcept;

/** Direct-sound onset (see the file comment); -1 for a silent response. */
int directOnset (const BinauralIr& ir) noexcept;

/** |normalised cross-correlation| maximum of l and r over [begin, end), lags
    within +-maxLag samples (r read at t + lag, inside the buffer); NaN when
    either ear has no energy there. lagOut (optional) receives the lag. */
double interauralCrossCorrelation (const std::vector<float>& l, const std::vector<float>& r, int begin, int end, int maxLag,
                                   int* lagOut = nullptr);

struct BinauralIrMetrics
{
    double onsetSeconds = 0.0;
    double iaccEarly = 0.0, iaccLate = 0.0;          // broadband; NaN: the window is empty (< -60 dB)
    std::array<double, 7> iaccEarlyBands {}, iaccLateBands {}; // per kIaccOctavesHz
    double itdMs = 0.0;                              // lag of the early IACC (+ = right ear lags)
    double drrDb = 0.0;                              // +inf without a reverberant part
    std::vector<double> bandsHz, leftDb, rightDb;    // 1/3-octave levels of the whole response
    bool valid = false;                              // false: silent response
};

/** All metrics of one response. */
BinauralIrMetrics analyseBinauralIr (const BinauralIr& ir, double sampleRate);

/** 1/3-octave levels (dB, mean |H|^2 per band; see the file comment) of a
    response at the given nominal centres. */
std::vector<double> thirdOctaveLevelsDb (const std::vector<float>& ir, double sampleRate, const std::vector<double>& bandsHz);

struct DiffuseField
{
    std::vector<double> bandsHz, levelDb, deviationDb;
    double rangeDb = 0.0, rmsDeviationDb = 0.0;
};

/** Diffuse-field response of a set of responses (both ears of each). */
DiffuseField diffuseField (const std::vector<BinauralIr>& irs, double sampleRate, double loHz = 100.0, double hiHz = 16000.0);

/** Largest minus smallest FFT magnitude (dB, 1/sampleRate resolution of a
    zero-padded 8192+ point transform) of one ear in [loHz, hiHz]: the
    peak-to-notch range of a comb. */
double peakToNotchDb (const std::vector<float>& ir, double sampleRate, double loHz, double hiHz);

/** Impulses in a stereo capture (e.g. an impulse per speaker rendered by
    `flubsound-cli process`): each starts at an onset at least minSpacing
    seconds after the previous one's and runs until 1 ms before the next
    (or the end). Onsets are where either ear reaches -20 dB re the file's
    peak after the spacing. */
std::vector<BinauralIr> splitImpulses (const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                                       double minSpacingSeconds = 0.1, std::vector<double>* onsetSeconds = nullptr);

/** The headphone virtualiser's response (the whole module after prepare():
    renderer, room, trim; level match and fold headroom off, since both are
    only a gain here) to a unit impulse on every layout channel in
    channelMask at once (bit c = input channel c; the LFE bit is ignored). */
BinauralIr virtualizerResponse (const VirtualizerParams& params, uint32_t channelMask, double sampleRate, int length);

// ---- HRTF-rendered ILD (docs/11 E24) ---------------------------------------

/** The azimuths of the method: 0 .. 180 degrees to the right in 15 degree
    steps (the parametric model is left / right symmetric). */
std::vector<float> focusIldAzimuths();

/** mono convolved with HeadphoneVirtualizer::parametricHrir (azimuth) of
    length hrirLength (stereo, the length of mono). */
std::vector<std::vector<float>> hrtfRenderedSource (const std::vector<float>& mono, float azimuthDeg, double sampleRate,
                                                    float headRadiusMm = 87.5f, int hrirLength = 512);

/** Per-band ILD (dB, left over right) of a stereo signal; NaN where a band
    of either channel holds no power. */
std::vector<double> bandIldDb (const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                               const std::vector<double>& bandsHz);

struct IldDeviation
{
    std::vector<double> bandsHz, referenceDb, measuredDb, deviationDb; // deviation = measured - reference
    double maxAbsDb = 0.0, maxAbsHz = 0.0; // largest |deviation| in [summaryLoHz, summaryHiHz] and its band
    double meanDb = 0.0;                   // mean deviation there
};

/** The ILD of `measured` against that of `reference` (both stereo), per
    1/3-octave band from loHz to hiHz; the summary over the focus region
    summaryLo .. summaryHi (default 1 - 8 kHz). */
IldDeviation ildDeviation (const std::vector<std::vector<float>>& reference, const std::vector<std::vector<float>>& measured,
                           double sampleRate, double loHz = 250.0, double hiHz = 16000.0, double summaryLoHz = 1000.0,
                           double summaryHiHz = 8000.0);

/** A stereo signal through the positional focus alone (StereoSpatializer at
    width 1, space 0, crossfeed 0, the given focus 0 .. 1; 512-sample blocks). */
std::vector<std::vector<float>> applyPositionalFocus (const std::vector<std::vector<float>>& stereo, float focus, double sampleRate);

/** The focus amounts of the method: off, 50 %, 100 %. */
inline constexpr std::array<float, 3> kFocusIldAmounts { 0.0f, 0.5f, 1.0f };

/** The method on one stereo source: its ILD deviation through the focus at
    each of kFocusIldAmounts (the source itself is the reference). */
std::array<IldDeviation, 3> focusIldDeviation (const std::vector<std::vector<float>>& source, double sampleRate);
} // namespace flub
