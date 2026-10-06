#include "SpectrumAnalyzer.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kFloorDb = -140.0f;
constexpr float kAttackSeconds = 0.012f;  // rise time constant
constexpr float kReleaseSeconds = 0.30f;  // fall time constant
constexpr float kPeakHoldSeconds = 1.2f;
constexpr float kPeakFallDbPerSecond = 14.0f;
constexpr float kTiltDbPerOctave = 4.5f;
constexpr double kIdleSeconds = 0.35; // no samples for this long: let the trace fall
constexpr float kWidthSeconds = 0.15f; // width view smoothing (both directions)
constexpr float kWidthStripShare = 0.2f; // width view: bottom fifth of the plot
// Spectrogram colour range (tilted levels, as drawn): typical music sits in the dim
// half, only the loudest bands reach the accent and the hot top.
constexpr float kSpectrogramFloorDb = -90.0f, kSpectrogramTopDb = -6.0f;

/** 1/6-octave bandwidth at 1 kHz (the scale's reference bandwidth). */
double bandwidthAt1k()
{
    const double edge = std::pow (2.0, 1.0 / 12.0);
    return 1000.0 * (edge - 1.0 / edge);
}

std::vector<float> hann (int size)
{
    // Periodic Hann window (exact 75 % overlap-add).
    std::vector<float> w (static_cast<size_t> (size));
    for (int i = 0; i < size; ++i)
        w[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / static_cast<float> (size));
    return w;
}

double midiFrequency (int midi)
{
    return 440.0 * std::pow (2.0, (midi - 69) / 12.0);
}

bool isBlackKey (int midi)
{
    const int n = ((midi % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer()
{
    // Opaque (fills with the panel colour) so 60 Hz repaints never have to
    // repaint the parent panel behind it.
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    setTitle ("Spectrum analyser");
    setDescription ("Input and output spectrum of the selected strip, 20 Hz to 20 kHz");

    window = hann (kFftSize);
    fftData.assign (static_cast<size_t> (2 * kFftSize), 0.0f);
    longWindow = hann (kLongWindow);
    longData.assign (static_cast<size_t> (2 * kLongFftSize), 0.0f);

    pointHz.resize (static_cast<size_t> (kNumPoints));
    tiltDb.resize (static_cast<size_t> (kNumPoints));
    longWeight.resize (static_cast<size_t> (kNumPoints));
    longPoints = 0;
    for (int i = 0; i < kNumPoints; ++i)
    {
        const double t = static_cast<double> (i) / (kNumPoints - 1);
        const double hz = kMinHz * std::pow (static_cast<double> (kMaxHz / kMinHz), t);
        pointHz[static_cast<size_t> (i)] = static_cast<float> (hz);
        tiltDb[static_cast<size_t> (i)] = kTiltDbPerOctave * static_cast<float> (std::log2 (hz / 1000.0));

        // Sharper lows: weight of the long analysis (smoothstep in log frequency).
        const double x = juce::jlimit (0.0, 1.0, std::log (hz / kCrossoverLoHz) / std::log (kCrossoverHiHz / kCrossoverLoHz));
        longWeight[static_cast<size_t> (i)] = static_cast<float> (1.0 - x * x * (3.0 - 2.0 * x));
        if (hz < kCrossoverHiHz)
            longPoints = i + 1;
    }

    const auto points = static_cast<size_t> (kNumPoints);
    for (auto* s : { &streams[0], &streams[1], &side })
    {
        s->history.assign (static_cast<size_t> (kFftSize), 0.0f);
        s->shortDb.assign (points, kFloorDb);
        s->analysisDb.assign (points, kFloorDb);
        s->displayDb.assign (points, kFloorDb);
        s->peakDb.assign (points, kFloorDb);
        s->peakAge.assign (points, 0.0f);
        s->longHistory.assign (static_cast<size_t> (kLongWindow), 0.0f);
        s->longDb.assign (points, kFloorDb);
    }
    differenceDb.assign (points, 0.0f);
    widthTarget.assign (points, 0.0f);
    widthDisplay.assign (points, 0.0f);
    frozenPre.assign (points, kFloorDb);
    frozenPost.assign (points, kFloorDb);
    rowScratch.assign (points, kFloorDb);
    rebuildBands();
    updateSpectrogramColours();
    spectrogram.clear();
}

// =============================================================================
// Data
// =============================================================================
void SpectrumAnalyzer::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0 || std::abs (newSampleRate - sampleRate) < 0.5)
        return;
    sampleRate = newSampleRate;
    rebuildBands();
    reset();
}

void SpectrumAnalyzer::rebuildBands()
{
    // 1/6 octave: band edges at fc * 2^(+-1/12). Where the band is narrower
    // than two bins (low frequencies) the magnitude is interpolated instead.
    const double binHz = sampleRate / kFftSize;
    const double edge = std::pow (2.0, 1.0 / 12.0);
    const int maxBin = kFftSize / 2 - 1;

    // dBFS scale (a full-scale sine reads 0 dB, noise its band power on the
    // same scale):
    //   20 log10 (4 / N)          Hann coherent gain 0.5: amplitude = 4 |X| / N
    //   - 10 log10 (1.5)          Hann equivalent noise bandwidth (bins): a tone's
    //                             |X|^2 summed over the band is 1.5x its peak bin's
    //   + 10 log10 (B1k / binHz)  mean bin power -> power of a 1/6-octave band
    //                             at 1 kHz (bandwidth B1k)
    // Bands hold whole bins, so a band's width only approximates its nominal
    // width: a 0 dBFS 1 kHz sine reads +0.3 dB at 44.1 kHz and +0.4 dB at
    // 48 kHz (its band spans 9 bins = 105 Hz instead of 115.6 Hz).
    const double bandwidth1k = bandwidthAt1k();
    const double hannEnbwBins = 1.5;
    calibrationDb = static_cast<float> (20.0 * std::log10 (4.0 / kFftSize) - 10.0 * std::log10 (hannEnbwBins)
                                        + 10.0 * std::log10 (bandwidth1k / binHz));

    bands.resize (static_cast<size_t> (kNumPoints));
    for (int i = 0; i < kNumPoints; ++i)
    {
        const double fc = pointHz[static_cast<size_t> (i)];
        const double lo = fc / edge / binHz, hi = fc * edge / binHz;
        auto& b = bands[static_cast<size_t> (i)];
        if (hi - lo < 2.0)
        {
            const double pos = juce::jlimit (0.0, static_cast<double> (maxBin - 1), fc / binHz);
            b.lo = static_cast<int> (std::floor (pos));
            b.frac = static_cast<float> (pos - b.lo);
            b.hi = b.lo - 1; // marks "interpolate"
        }
        else
        {
            b.lo = juce::jlimit (1, maxBin, static_cast<int> (std::ceil (lo)));
            b.hi = juce::jlimit (b.lo, maxBin, static_cast<int> (std::floor (hi)));
        }
    }

    // Sharper lows: the same density scale for the decimated analysis. The
    // window is kLongWindow decimated samples (zero padding changes neither
    // the amplitude nor the noise bandwidth), its bins fsd / kLongWindow wide.
    decimation = juce::jlimit (1, 32, juce::roundToInt (sampleRate / kLongTargetRate));
    longHop = juce::jmax (1, kHop / decimation);
    const double decimatedRate = sampleRate / decimation;
    longCalibrationDb = static_cast<float> (20.0 * std::log10 (4.0 / kLongWindow) - 10.0 * std::log10 (hannEnbwBins)
                                            + 10.0 * std::log10 (bandwidth1k / (decimatedRate / kLongWindow)));

    // Anti-alias filter: 4th-order Butterworth low-pass at fsd / 10 (two
    // RBJ biquads, Q 0.5412 / 1.3066): at 48 kHz 800 Hz, about -79 dB where
    // 7.7 .. 8.3 kHz would fold into 0 .. 300 Hz, and flat below 300 Hz
    // (-0.002 dB).
    const double cutoff = decimatedRate * 0.1;
    const double w0 = 2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate;
    const double qs[] = { 0.54119610014619698, 1.3065629648763766 };
    for (size_t s = 0; s < 2; ++s)
    {
        const double alpha = std::sin (w0) / (2.0 * qs[s]);
        const double cw = std::cos (w0);
        const double a0 = 1.0 + alpha;
        lpCoeffs[5 * s + 0] = (1.0 - cw) * 0.5 / a0;
        lpCoeffs[5 * s + 1] = (1.0 - cw) / a0;
        lpCoeffs[5 * s + 2] = (1.0 - cw) * 0.5 / a0;
        lpCoeffs[5 * s + 3] = -2.0 * cw / a0;
        lpCoeffs[5 * s + 4] = (1.0 - alpha) / a0;
    }
}

double SpectrumAnalyzer::getLowResolutionBandwidthHz() const noexcept
{
    return 1.5 * (sampleRate / decimation) / kLongWindow;
}

void SpectrumAnalyzer::pushInto (Stream& s, const float* samples, int numSamples, bool decimate)
{
    for (int i = 0; i < numSamples; ++i)
    {
        s.history[static_cast<size_t> (s.writePos)] = samples[i];
        s.writePos = (s.writePos + 1) & (kFftSize - 1);
    }
    s.sinceHop += numSamples;
    s.filled = juce::jmin (kFftSize, s.filled + numSamples);
    s.idleSeconds = 0.0;
    s.fresh = true;

    if (! decimate)
        return;
    for (int i = 0; i < numSamples; ++i)
    {
        double y = samples[i];
        for (size_t b = 0; b < 2; ++b)
        {
            const double* c = &lpCoeffs[5 * b];
            double* z = &s.lpState[2 * b];
            const double out = c[0] * y + z[0];
            z[0] = c[1] * y - c[3] * out + z[1];
            z[1] = c[2] * y - c[4] * out;
            y = out;
        }
        if (++s.decimPhase >= decimation)
        {
            s.decimPhase = 0;
            s.longHistory[static_cast<size_t> (s.longWrite)] = static_cast<float> (y);
            s.longWrite = (s.longWrite + 1) & (kLongWindow - 1);
            ++s.longSinceHop;
            s.longFilled = juce::jmin (kLongWindow, s.longFilled + 1);
        }
    }
    // Silence decays the filter state towards denormals: flush it.
    for (auto& z : s.lpState)
        if (std::abs (z) < 1.0e-30)
            z = 0.0;
}

void SpectrumAnalyzer::push (bool post, const float* samples, int numSamples)
{
    // The decimated history only runs while Sharper lows is on (no extra work otherwise).
    pushInto (streams[post ? 1 : 0], samples, numSamples, sharpLows);
}

void SpectrumAnalyzer::pushSide (const float* samples, int numSamples)
{
    if (widthOn)
        pushInto (side, samples, numSamples, false);
}

void SpectrumAnalyzer::reset()
{
    for (auto* s : { &streams[0], &streams[1], &side })
    {
        std::fill (s->history.begin(), s->history.end(), 0.0f);
        std::fill (s->shortDb.begin(), s->shortDb.end(), kFloorDb);
        std::fill (s->analysisDb.begin(), s->analysisDb.end(), kFloorDb);
        std::fill (s->displayDb.begin(), s->displayDb.end(), kFloorDb);
        std::fill (s->peakDb.begin(), s->peakDb.end(), kFloorDb);
        std::fill (s->peakAge.begin(), s->peakAge.end(), 0.0f);
        std::fill (s->longHistory.begin(), s->longHistory.end(), 0.0f);
        std::fill (s->longDb.begin(), s->longDb.end(), kFloorDb);
        s->writePos = s->sinceHop = s->filled = 0;
        s->longWrite = s->longSinceHop = s->longFilled = s->decimPhase = 0;
        s->lpState.fill (0.0);
        s->longReady = false;
        s->idleSeconds = 0.0;
        s->fresh = false;
    }
    std::fill (differenceDb.begin(), differenceDb.end(), 0.0f);
    std::fill (widthTarget.begin(), widthTarget.end(), 0.0f);
    std::fill (widthDisplay.begin(), widthDisplay.end(), 0.0f);
    spectrogram.clear();
    anyData = false;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::analyse (Stream& s)
{
    // Oldest -> newest, windowed.
    for (int i = 0; i < kFftSize; ++i)
        fftData[static_cast<size_t> (i)] = s.history[static_cast<size_t> ((s.writePos + i) & (kFftSize - 1))] * window[static_cast<size_t> (i)];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    for (int i = 0; i < kNumPoints; ++i)
    {
        const auto& b = bands[static_cast<size_t> (i)];
        double power = 0.0;
        if (b.hi < b.lo)
        {
            const double m = fftData[static_cast<size_t> (b.lo)] + (fftData[static_cast<size_t> (b.lo + 1)] - fftData[static_cast<size_t> (b.lo)]) * b.frac;
            power = m * m;
        }
        else
        {
            for (int k = b.lo; k <= b.hi; ++k)
                power += static_cast<double> (fftData[static_cast<size_t> (k)]) * fftData[static_cast<size_t> (k)];
            power /= (b.hi - b.lo + 1);
        }
        s.shortDb[static_cast<size_t> (i)] = juce::jmax (kFloorDb, static_cast<float> (10.0 * std::log10 (power + 1.0e-24)) + calibrationDb);
    }
}

void SpectrumAnalyzer::analyseLong (Stream& s)
{
    for (int i = 0; i < kLongWindow; ++i)
        longData[static_cast<size_t> (i)] = s.longHistory[static_cast<size_t> ((s.longWrite + i) & (kLongWindow - 1))] * longWindow[static_cast<size_t> (i)];
    std::fill (longData.begin() + kLongWindow, longData.end(), 0.0f); // zero padding x2 (+ the transform's scratch half)
    longFft.performFrequencyOnlyForwardTransform (longData.data(), true);

    // No band averaging: the magnitude interpolated between the padded bins
    // (half a window bin apart, so a tone between two bins reads at most
    // 0.35 dB low).
    const double padBinHz = (sampleRate / decimation) / kLongFftSize;
    const int maxBin = kLongFftSize / 2 - 1;
    for (int i = 0; i < longPoints; ++i)
    {
        const double pos = juce::jlimit (0.0, static_cast<double> (maxBin - 1), pointHz[static_cast<size_t> (i)] / padBinHz);
        const auto k = static_cast<size_t> (pos);
        const double frac = pos - static_cast<double> (k);
        const double m = longData[k] + (longData[k + 1] - longData[k]) * frac;
        s.longDb[static_cast<size_t> (i)] = juce::jmax (kFloorDb, static_cast<float> (10.0 * std::log10 (m * m + 1.0e-24)) + longCalibrationDb);
    }
}

void SpectrumAnalyzer::combine (Stream& s)
{
    if (! sharpLows || ! s.longReady)
    {
        std::copy (s.shortDb.begin(), s.shortDb.end(), s.analysisDb.begin());
        return;
    }
    // Crossfade in dB: long analysis below kCrossoverLoHz, the main one above kCrossoverHiHz.
    for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
    {
        const float w = static_cast<int> (i) < longPoints ? longWeight[i] : 0.0f;
        s.analysisDb[i] = s.shortDb[i] + (s.longDb[i] - s.shortDb[i]) * w;
    }
}

size_t SpectrumAnalyzer::pointIndex (double hz) const noexcept
{
    // Display points are log-spaced: the nearest one in log frequency.
    const double t = std::log (juce::jlimit (static_cast<double> (kMinHz), static_cast<double> (kMaxHz), hz) / kMinHz)
                     / std::log (static_cast<double> (kMaxHz / kMinHz));
    return static_cast<size_t> (juce::jlimit (0, kNumPoints - 1, juce::roundToInt (t * (kNumPoints - 1))));
}

float SpectrumAnalyzer::getBandLevelDb (bool post, double hz) const noexcept
{
    return streams[post ? 1 : 0].analysisDb[pointIndex (hz)];
}

float SpectrumAnalyzer::getDisplayLevelDb (bool post, double hz) const noexcept
{
    return streams[post ? 1 : 0].displayDb[pointIndex (hz)];
}

float SpectrumAnalyzer::getDifferenceDb (double hz) const noexcept
{
    return differenceDb[pointIndex (hz)];
}

float SpectrumAnalyzer::getStereoWidth (double hz) const noexcept
{
    return widthTarget[pointIndex (hz)];
}

void SpectrumAnalyzer::advance (double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    const float attack = 1.0f - std::exp (-dt / kAttackSeconds);
    const float release = 1.0f - std::exp (-dt / kReleaseSeconds);
    bool moved = false;
    bool postHopped = false;

    for (size_t si = 0; si < streams.size(); ++si)
    {
        auto& s = streams[si];
        bool hopped = false;
        // Run every pending hop (normally 0 or 1 per frame; bounded after stalls).
        if (s.filled >= kFftSize / 2 && s.sinceHop >= kHop)
        {
            analyse (s);
            s.sinceHop = juce::jmin (s.sinceHop - kHop, kHop - 1);
            anyData = true;
            hopped = true;
        }
        if (sharpLows && s.longFilled >= kLongWindow / 2 && s.longSinceHop >= longHop)
        {
            analyseLong (s);
            s.longSinceHop = juce::jmin (s.longSinceHop - longHop, longHop - 1);
            s.longReady = true;
            hopped = true;
        }
        if (hopped)
            combine (s);
        postHopped = postHopped || (si == 1 && hopped);

        s.idleSeconds += dt;
        if (s.idleSeconds > kIdleSeconds)
            std::fill (s.analysisDb.begin(), s.analysisDb.end(), kFloorDb); // no signal: fall away

        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
        {
            auto& d = s.displayDb[i];
            const float target = s.analysisDb[i];
            const float before = d;
            d += (target - d) * (target > d ? attack : release);

            auto& peak = s.peakDb[i];
            auto& age = s.peakAge[i];
            if (d >= peak)
            {
                peak = d;
                age = 0.0f;
            }
            else
            {
                age += dt;
                if (age > kPeakHoldSeconds)
                    peak = juce::jmax (d, peak - kPeakFallDbPerSecond * dt);
            }
            moved = moved || std::abs (d - before) > 0.01f;
        }
    }

    // ---- Stereo width: side against the post mid, both from the main analysis ----
    if (widthOn)
    {
        if (side.filled >= kFftSize / 2 && side.sinceHop >= kHop)
        {
            analyse (side);
            side.sinceHop = juce::jmin (side.sinceHop - kHop, kHop - 1);
            for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
                widthTarget[i] = widthFromLevels (streams[1].shortDb[i], side.shortDb[i]);
        }
        side.idleSeconds += dt;
        if (side.idleSeconds > kIdleSeconds || streams[1].idleSeconds > kIdleSeconds)
            std::fill (widthTarget.begin(), widthTarget.end(), 0.0f);
        const float k = 1.0f - std::exp (-dt / kWidthSeconds);
        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
        {
            const float before = widthDisplay[i];
            widthDisplay[i] += (widthTarget[i] - widthDisplay[i]) * k;
            moved = moved || std::abs (widthDisplay[i] - before) > 0.002f;
        }
    }

    if (differenceOn && moved)
        computeDifference (streams[0].displayDb.data(), streams[1].displayDb.data(), differenceDb.data(), kNumPoints);

    // ---- Spectrogram: one row per post hop (the analysed values, tilt as drawn) ----
    bool rowWritten = false;
    if (spectrogramOn && postHopped)
    {
        const auto& values = streams[1].analysisDb;
        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
            rowScratch[i] = values[i] + (tilt ? tiltDb[i] : 0.0f);
        spectrogram.writeRow (rowScratch.data(), kNumPoints, kSpectrogramFloorDb, kSpectrogramTopDb);
        rowWritten = true;
    }

    if (moved)
        rebuildPaths();
    if (moved || rowWritten)
        repaint (plot.expanded (2.0f).getSmallestIntegerContainer());
}

// =============================================================================
// Pure helpers
// =============================================================================
juce::String SpectrumAnalyzer::noteName (double hz)
{
    if (! (hz >= 8.0 && hz <= 30000.0))
        return {};
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double semitones = 12.0 * std::log2 (hz / 440.0);
    const auto nearest = static_cast<int> (std::lround (semitones));
    const int midi = 69 + nearest;
    const auto cents = static_cast<int> (std::lround (100.0 * (semitones - nearest)));
    const int octave = midi / 12 - 1; // midi >= 0 from 8 Hz up
    return juce::String (names[midi % 12]) + juce::String (octave) + " " + (cents < 0 ? "-" : "+") + juce::String (std::abs (cents)) + "c";
}

juce::String SpectrumAnalyzer::frequencyText (double hz)
{
    if (hz < 100.0)
        return juce::String (hz, 1) + " Hz";
    if (hz < 999.5)
        return juce::String (juce::roundToInt (hz)) + " Hz";
    if (hz < 9995.0)
        return juce::String (hz / 1000.0, 2) + " kHz";
    return juce::String (hz / 1000.0, 1) + " kHz";
}

juce::String SpectrumAnalyzer::describeFrequency (double hz)
{
    const auto note = noteName (hz);
    const auto freq = frequencyText (hz);
    return note.isEmpty() ? freq : note + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + freq;
}

void SpectrumAnalyzer::computeDifference (const float* pre, const float* post, float* out, int n, int halfWidth, float gateDb)
{
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        int count = 0;
        for (int j = juce::jmax (0, i - halfWidth); j <= juce::jmin (n - 1, i + halfWidth); ++j)
        {
            if (juce::jmax (pre[j], post[j]) > gateDb)
            {
                sum += post[j] - pre[j];
                ++count;
            }
        }
        out[i] = count > 0 ? static_cast<float> (sum / count) : 0.0f;
    }
}

float SpectrumAnalyzer::widthFromLevels (float midDb, float sideDb, float gateDb) noexcept
{
    if (juce::jmax (midDb, sideDb) < gateDb)
        return 0.0f;
    return 1.0f / (1.0f + std::pow (10.0f, (midDb - sideDb) / 10.0f));
}

// =============================================================================
// Options
// =============================================================================
void SpectrumAnalyzer::setShowPre (bool shouldShow)
{
    showPre = shouldShow;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setShowPost (bool shouldShow)
{
    showPost = shouldShow;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setTiltEnabled (bool shouldTilt)
{
    tilt = shouldTilt;
    frozenPathsDirty = true;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setPeakHoldEnabled (bool shouldHold)
{
    peakHold = shouldHold;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setDifferenceEnabled (bool shouldShow)
{
    if (differenceOn == shouldShow)
        return;
    differenceOn = shouldShow;
    computeDifference (streams[0].displayDb.data(), streams[1].displayDb.data(), differenceDb.data(), kNumPoints);
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setSharpLowsEnabled (bool shouldUse)
{
    if (sharpLows == shouldUse)
        return;
    sharpLows = shouldUse;
    for (auto& s : streams)
    {
        // The decimated history starts afresh (it did not run while off).
        std::fill (s.longHistory.begin(), s.longHistory.end(), 0.0f);
        std::fill (s.longDb.begin(), s.longDb.end(), kFloorDb);
        s.longWrite = s.longSinceHop = s.longFilled = s.decimPhase = 0;
        s.lpState.fill (0.0);
        s.longReady = false;
        if (anyData)
            combine (s); // off: back to the main analysis at once
    }
}

void SpectrumAnalyzer::setWidthEnabled (bool shouldShow)
{
    if (widthOn == shouldShow)
        return;
    widthOn = shouldShow;
    std::fill (side.history.begin(), side.history.end(), 0.0f);
    side.writePos = side.sinceHop = side.filled = 0;
    side.idleSeconds = 0.0;
    std::fill (widthTarget.begin(), widthTarget.end(), 0.0f);
    std::fill (widthDisplay.begin(), widthDisplay.end(), 0.0f);
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setSpectrogramEnabled (bool shouldShow)
{
    if (spectrogramOn == shouldShow)
        return;
    spectrogramOn = shouldShow;
    if (spectrogramOn)
    {
        updateSpectrogramColours();
        spectrogram.clear();
    }
    gridImage = {}; // the left axis shows time instead of level
    repaint();
}

void SpectrumAnalyzer::setPianoKeysEnabled (bool shouldShow)
{
    if (keysOn == shouldShow)
        return;
    keysOn = shouldShow;
    rebuildPaths(); // the width view sits on the keys
    repaint();
}

void SpectrumAnalyzer::setEqRangeDb (float rangeDb)
{
    eqRangeDb = juce::jlimit (3.0f, 24.0f, rangeDb);
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::freeze()
{
    frozenPost = streams[1].displayDb;
    frozenHasPre = showPre;
    if (frozenHasPre)
        frozenPre = streams[0].displayDb;
    frozen = true;
    frozenPathsDirty = true;
    rebuildFrozenPaths();
    repaint();
}

void SpectrumAnalyzer::clearFreeze()
{
    frozen = false;
    frozenPreDashes.clear();
    frozenPostDashes.clear();
    repaint();
}

void SpectrumAnalyzer::setHoverFrequency (double hz)
{
    if (hz == hoverHz)
        return;
    const auto keyOf = [] (double f) { return f > 0.0 ? static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (f / 440.0))) : -1; };
    const bool keyChanged = keyOf (hz) != keyOf (hoverHz);
    hoverHz = hz;
    if (keysOn && keyChanged)
        repaint (plot.withTop (plot.getBottom() - kKeysHeight - 14.0f).expanded (2.0f).getSmallestIntegerContainer());
}

// =============================================================================
// Geometry / paths
// =============================================================================
float SpectrumAnalyzer::xForFrequency (double hz) const noexcept
{
    const double t = std::log (juce::jmax (1.0, hz) / kMinHz) / std::log (static_cast<double> (kMaxHz / kMinHz));
    return plot.getX() + static_cast<float> (t) * plot.getWidth();
}

double SpectrumAnalyzer::frequencyForX (float x) const noexcept
{
    const double t = plot.getWidth() > 0.0f ? (x - plot.getX()) / plot.getWidth() : 0.0;
    return kMinHz * std::pow (static_cast<double> (kMaxHz / kMinHz), t);
}

float SpectrumAnalyzer::yForDb (float db) const noexcept
{
    const float t = (juce::jlimit (kMinDb - 12.0f, kMaxDb + 6.0f, db) - kMinDb) / (kMaxDb - kMinDb);
    return plot.getBottom() - t * plot.getHeight();
}

float SpectrumAnalyzer::yForGain (float db, float rangeDb) const noexcept
{
    const float t = juce::jlimit (-1.08f, 1.08f, db / rangeDb);
    return plot.getCentreY() - t * plot.getHeight() * 0.5f * 0.92f;
}

float SpectrumAnalyzer::traceY (bool post, double hz) const noexcept
{
    const auto i = pointIndex (hz);
    return juce::jmin (plot.getBottom(), yForDb (streams[post ? 1 : 0].displayDb[i] + (tilt ? tiltDb[i] : 0.0f)));
}

void SpectrumAnalyzer::buildTrace (const std::vector<float>& values, juce::Path& line, juce::Path* fill) const
{
    line.clear();
    if (fill != nullptr)
        fill->clear();
    if (plot.isEmpty())
        return;

    const float bottom = plot.getBottom();
    for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
    {
        const float db = values[i] + (tilt ? tiltDb[i] : 0.0f);
        const float y = juce::jmin (bottom, yForDb (db));
        if (i == 0)
            line.startNewSubPath (pointX[i], y);
        else
            line.lineTo (pointX[i], y);
    }

    if (fill != nullptr)
    {
        *fill = line;
        fill->lineTo (pointX.back(), bottom);
        fill->lineTo (pointX.front(), bottom);
        fill->closeSubPath();
    }
}

void SpectrumAnalyzer::rebuildPaths()
{
    const bool live = anyData && ! plot.isEmpty();
    if (showPre && live)
        buildTrace (streams[0].displayDb, preLine, &preFill);
    else
    {
        preLine.clear();
        preFill.clear();
    }

    if (showPost && live)
    {
        buildTrace (streams[1].displayDb, postLine, &postFill);
        if (peakHold)
            buildTrace (streams[1].peakDb, peakLine, nullptr);
        else
            peakLine.clear();
    }
    else
    {
        postLine.clear();
        postFill.clear();
        peakLine.clear();
    }

    // ---- Difference: post - pre on the EQ gain axis ----
    diffLine.clear();
    diffFill.clear();
    if (differenceOn && live)
    {
        const float zeroY = yForGain (0.0f, eqRangeDb);
        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
        {
            const float y = yForGain (differenceDb[i], eqRangeDb);
            if (i == 0)
                diffLine.startNewSubPath (pointX[i], y);
            else
                diffLine.lineTo (pointX[i], y);
        }
        diffFill = diffLine;
        diffFill.lineTo (pointX.back(), zeroY);
        diffFill.lineTo (pointX.front(), zeroY);
        diffFill.closeSubPath();
    }

    // ---- Stereo width: bottom fifth of the plot (above the piano keys) ----
    widthFill.clear();
    widthLine.clear();
    if (widthOn && live)
    {
        const float base = plot.getBottom() - (keysOn ? kKeysHeight + 12.0f : 0.0f);
        const float height = plot.getHeight() * kWidthStripShare;
        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
        {
            const float y = base - juce::jlimit (0.0f, 1.0f, widthDisplay[i]) * height;
            if (i == 0)
                widthLine.startNewSubPath (pointX[i], y);
            else
                widthLine.lineTo (pointX[i], y);
        }
        widthFill = widthLine;
        widthFill.lineTo (pointX.back(), base);
        widthFill.lineTo (pointX.front(), base);
        widthFill.closeSubPath();
    }

    if (frozen && frozenPathsDirty)
        rebuildFrozenPaths();
}

void SpectrumAnalyzer::rebuildFrozenPaths()
{
    frozenPreDashes.clear();
    frozenPostDashes.clear();
    frozenPathsDirty = false;
    if (! frozen || plot.isEmpty())
        return;

    const float dashes[] = { 5.0f, 3.0f };
    juce::Path line;
    buildTrace (frozenPost, line, nullptr);
    juce::PathStrokeType (1.3f).createDashedStroke (frozenPostDashes, line, dashes, 2);
    if (frozenHasPre)
    {
        buildTrace (frozenPre, line, nullptr);
        juce::PathStrokeType (1.0f).createDashedStroke (frozenPreDashes, line, dashes, 2);
    }
}

void SpectrumAnalyzer::rebuildKeys()
{
    whiteKeys.clear();
    blackKeys.clear();
    keySeparators.clear();
    if (plot.isEmpty())
        return;

    for (int m = kKeysLowMidi; m <= kKeysHighMidi; ++m)
    {
        const auto key = getKeyBounds (m);
        if (isBlackKey (m))
        {
            blackKeys.addRectangle (key);
            continue;
        }
        whiteKeys.addRectangle (key);
        if (m < kKeysHighMidi)
            keySeparators.addRectangle (key.getRight() - 0.5f, key.getY(), 1.0f, key.getHeight());
    }
}

juce::Rectangle<float> SpectrumAnalyzer::getKeyBounds (int midi) const noexcept
{
    if (midi < kKeysLowMidi || midi > kKeysHighMidi || plot.isEmpty())
        return {};
    const float top = plot.getBottom() - kKeysHeight;
    const double halfStep = std::pow (2.0, 1.0 / 24.0), f = midiFrequency (midi);
    if (isBlackKey (midi))
        return juce::Rectangle<float>::leftTopRightBottom (xForFrequency (f / halfStep), top, xForFrequency (f * halfStep),
                                                           top + kKeysHeight * 0.62f);
    const double lo = (midi > kKeysLowMidi && isBlackKey (midi - 1)) ? midiFrequency (midi - 1) : f / halfStep;
    const double hi = (midi < kKeysHighMidi && isBlackKey (midi + 1)) ? midiFrequency (midi + 1) : f * halfStep;
    return juce::Rectangle<float>::leftTopRightBottom (xForFrequency (lo), top, xForFrequency (hi), plot.getBottom());
}

void SpectrumAnalyzer::resized()
{
    auto r = getLocalBounds().toFloat();
    plot = juce::Rectangle<float> (r.getX() + kLeftInset, r.getY() + kTopInset, r.getWidth() - kLeftInset - kRightInset,
                                   r.getHeight() - kTopInset - kBottomInset);
    pointX.resize (static_cast<size_t> (kNumPoints));
    for (size_t i = 0; i < pointX.size(); ++i)
        pointX[i] = xForFrequency (pointHz[i]);
    gridImage = {};
    frozenPathsDirty = true;
    rebuildKeys();
    rebuildPaths();
}

void SpectrumAnalyzer::moved()
{
    gridImage = {}; // the panel gradient behind it depends on the position
}

void SpectrumAnalyzer::lookAndFeelChanged()
{
    gridImage = {};
    updateSpectrogramColours();
    repaint();
}

void SpectrumAnalyzer::updateSpectrogramColours()
{
    spectrogramAccent = Theme::accent (*this);
    spectrogram.setColours (Palette::well, spectrogramAccent, Palette::text);
}

void SpectrumAnalyzer::renderGrid (float scale)
{
    const int w = juce::roundToInt (static_cast<float> (getWidth()) * scale);
    const int h = juce::roundToInt (static_cast<float> (getHeight()) * scale);
    gridImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
    gridScale = scale;
    gridForSpectrogram = spectrogramOn;

    juce::Graphics g (gridImage);
    g.addTransform (juce::AffineTransform::scale (scale));

    // The parent panel's fill behind the axis labels (the component is opaque), then the plot well.
    if (auto* parent = getParentComponent())
        g.setGradientFill (Theme::panelFill (getLocalArea (parent, parent->getLocalBounds()).toFloat()));
    else
        g.setColour (Palette::panel);
    g.fillAll();
    g.setColour (Palette::well);
    g.fillRoundedRectangle (plot.expanded (1.0f), 6.0f);

    // Frequency grid: faint minor lines, brighter decades, labelled majors.
    for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
    {
        for (int m = 1; m <= 9; ++m)
        {
            const double f = decade * m;
            if (f < kMinHz || f > kMaxHz)
                continue;
            const float x = std::round (xForFrequency (f)) + 0.5f;
            g.setColour (m == 1 ? Palette::gridMajor : Palette::gridMinor);
            g.drawVerticalLine (static_cast<int> (x), plot.getY(), plot.getBottom());
        }
    }

    g.setFont (Theme::font (10.5f));
    g.setColour (Palette::faint.brighter (0.25f));
    for (const double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = xForFrequency (f);
        const auto text = f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f));
        auto labelArea = juce::Rectangle<float> (x - 22.0f, plot.getBottom() + 4.0f, 44.0f, 14.0f);
        if (f == 20.0)
            labelArea = labelArea.withX (x - 2.0f);
        else if (f == 20000.0)
            labelArea = labelArea.withRightX (x + 2.0f);
        g.drawText (text, labelArea, f == 20.0 ? juce::Justification::centredLeft
                                                : (f == 20000.0 ? juce::Justification::centredRight : juce::Justification::centred));
    }

    if (spectrogramOn)
    {
        // Spectrogram: the left axis is time (newest at the top).
        const double rowSeconds = juce::jmax (static_cast<double> (kHop) / sampleRate, 1.0 / 60.0);
        const double span = rowSeconds * kSpectrogramRows;
        g.setColour (Palette::faint);
        for (int s = 0; s <= static_cast<int> (span); ++s)
        {
            if (s % 2 != 0 && s != 0)
                continue;
            const float y = plot.getY() + static_cast<float> (s / span) * plot.getHeight();
            const auto text = s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s";
            g.drawText (text, juce::Rectangle<float> (plot.getX() - kLeftInset, y - (s == 0 ? 0.0f : 7.0f), kLeftInset - 6.0f, 14.0f),
                        juce::Justification::centredRight);
        }
        return;
    }

    // Level grid (left axis): 12 dB steps, coarser when the plot is short.
    const float pixelsPer12Db = plot.getHeight() * 12.0f / (kMaxDb - kMinDb);
    const float step = pixelsPer12Db >= 16.0f ? 12.0f : (pixelsPer12Db >= 8.0f ? 24.0f : 42.0f);
    for (float db = kMaxDb; db >= kMinDb; db -= step)
    {
        const float y = std::round (yForDb (db)) + 0.5f;
        g.setColour (Palette::grid.interpolatedWith (Palette::gridMinor, 0.5f));
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        g.setColour (Palette::faint);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (plot.getX() - kLeftInset, y - 7.0f, kLeftInset - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
}

void SpectrumAnalyzer::paintKeys (juce::Graphics& g, juce::Colour accent) const
{
    const float top = plot.getBottom() - kKeysHeight;
    g.setColour (Palette::muted.withAlpha (0.55f));
    g.fillPath (whiteKeys);

    // The hovered note's key.
    juce::Rectangle<float> lit;
    bool litBlack = false;
    if (hoverHz > 0.0)
    {
        const auto m = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (hoverHz / 440.0)));
        lit = getKeyBounds (m);
        litBlack = isBlackKey (m);
    }
    if (! lit.isEmpty() && ! litBlack)
    {
        g.setColour (accent.withAlpha (0.9f));
        g.fillRect (lit);
    }
    g.setColour (Palette::well);
    g.fillPath (keySeparators);
    g.fillPath (blackKeys);
    if (! lit.isEmpty() && litBlack)
    {
        g.setColour (accent.withAlpha (0.9f));
        g.fillRect (lit);
    }

    // C labels above the strip.
    g.setFont (Theme::font (9.0f));
    g.setColour (Palette::faint);
    for (int m = kKeysLowMidi; m <= kKeysHighMidi; m += 12)
    {
        const float x = xForFrequency (midiFrequency (m) / std::pow (2.0, 1.0 / 24.0));
        g.drawText ("C" + juce::String (m / 12 - 1), juce::Rectangle<float> (x + 1.0f, top - 12.0f, 24.0f, 11.0f),
                    juce::Justification::centredLeft, false);
    }
}

// =============================================================================
void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (gridImage.isNull() || std::abs (scale - gridScale) > 0.01f || gridForSpectrogram != spectrogramOn)
        renderGrid (scale);
    g.drawImageTransformed (gridImage, juce::AffineTransform::scale (1.0f / gridScale));

    const auto accent = Theme::accent (*this);

    if (! anyData)
    {
        if (keysOn)
            paintKeys (g, accent);
        g.setColour (Palette::faint);
        g.setFont (Theme::font (12.0f));
        g.drawText ("Waiting for audio on this strip", plot.withHeight (juce::jmin (plot.getHeight() * 0.4f, 60.0f)), juce::Justification::centred);
        return;
    }

    g.saveState();
    g.reduceClipRegion (plot.getSmallestIntegerContainer());

    if (spectrogramOn)
    {
        if (accent != spectrogramAccent)
            updateSpectrogramColours(); // a mode switch: new rows in the new accent
        // Column i is display point i: its centre at pointX[i].
        const float column = plot.getWidth() / static_cast<float> (kNumPoints - 1);
        spectrogram.draw (g, plot.withX (plot.getX() - column * 0.5f).withWidth (plot.getWidth() + column));
        // Decade lines over the image keep the frequency axis readable.
        g.setColour (Palette::gridMajor.withAlpha (0.55f));
        for (const double f : { 100.0, 1000.0, 10000.0 })
            g.drawVerticalLine (static_cast<int> (std::round (xForFrequency (f))), plot.getY(), plot.getBottom());
    }
    else
    {
        if (! preFill.isEmpty())
        {
            g.setGradientFill (juce::ColourGradient (Palette::muted.withAlpha (0.22f), 0.0f, plot.getY(), Palette::muted.withAlpha (0.03f), 0.0f,
                                                     plot.getBottom(), false));
            g.fillPath (preFill);
            g.setColour (Palette::muted.withAlpha (0.45f));
            g.strokePath (preLine, juce::PathStrokeType (1.0f));
        }
    }

    // Stereo width (both views): the other mode's accent, translucent.
    if (! widthFill.isEmpty())
    {
        const auto widthColour = accent == Palette::magenta ? Palette::teal : Palette::magenta;
        const float base = plot.getBottom() - (keysOn ? kKeysHeight + 12.0f : 0.0f);
        const float height = plot.getHeight() * kWidthStripShare;
        g.setColour (widthColour.withAlpha (0.16f));
        g.fillPath (widthFill);
        g.setColour (widthColour.withAlpha (0.7f));
        g.strokePath (widthLine, juce::PathStrokeType (1.0f));
        // Guide at 0.5 (as wide as uncorrelated channels) and the caption.
        const float half = base - height * 0.5f;
        g.setColour (widthColour.withAlpha (0.35f));
        for (float x = plot.getX() + 2.0f; x < plot.getRight(); x += 6.0f)
            g.fillRect (x, half, 3.0f, 1.0f);
        g.setFont (Theme::font (9.0f, true));
        g.drawText ("WIDTH", juce::Rectangle<float> (plot.getX() + 4.0f, base - height, 60.0f, 11.0f), juce::Justification::centredLeft, false);
    }

    if (! spectrogramOn)
    {
        if (! postLine.isEmpty())
        {
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.20f), 0.0f, plot.getY(), accent.withAlpha (0.0f), 0.0f, plot.getBottom(),
                                                     false));
            g.fillPath (postFill);

            // Glow: a wide translucent stroke under the crisp line.
            g.setColour (accent.withAlpha (0.15f));
            g.strokePath (postLine, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent);
            g.strokePath (postLine, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (! peakLine.isEmpty())
        {
            g.setColour (accent.withAlpha (0.38f));
            g.strokePath (peakLine, juce::PathStrokeType (1.0f));
        }

        // Frozen reference: dashed, muted.
        if (frozen)
        {
            g.setColour (Palette::muted.withAlpha (0.45f));
            g.fillPath (frozenPreDashes);
            g.setColour (Palette::text.withAlpha (0.62f));
            g.fillPath (frozenPostDashes);
        }
    }

    // Difference (both views): green, on the EQ gain axis.
    if (! diffLine.isEmpty())
    {
        g.setColour (Palette::green.withAlpha (0.10f));
        g.fillPath (diffFill);
        g.setColour (Palette::green.withAlpha (0.18f));
        g.strokePath (diffLine, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Palette::green);
        g.strokePath (diffLine, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (keysOn)
        paintKeys (g, accent);

    g.restoreState();
}
} // namespace flub::app::ui
