// Device correction (docs/11 E15) and the headroom predictor (docs/11 E11):
// AutoEQ / Equalizer APO text import (accepted forms, refusals, warnings,
// round trip), the rendered response against an independent RBJ cookbook
// reference, the automatic preamp (no sine and no 0 dBFS pink clips after
// it), click-free curve changes, bypass / compare, allocation freedom, NaN
// recovery, block-size invariance and the MixEngine hook (after the strip
// sum, before the master limiter, kept across configure / configureFrom).
// Every curve here is synthetic: no third-party measurement data is bundled.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/DeviceCorrection.h"
#include "flub/engine/MixEngine.h"
#include "flub/io/ParametricEqText.h"

#include <array>
#include <complex>
#include <limits>
#include <memory>

using namespace flub;
using namespace flubtest;

#if defined(FLUB_RTSAN)
static_assert (std::is_same_v<decltype (&DeviceCorrection::process), void (DeviceCorrection::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DeviceCorrection::reset), void (DeviceCorrection::*)() noexcept FLUB_NONBLOCKING>);
#endif

namespace
{
constexpr double kFs = 48000.0;

// A synthetic curve in AutoEQ's ParametricEQ.txt layout (made-up values of
// a typical over-ear correction: bass shelf, 3-8 kHz peaks and dips).
constexpr const char* kSyntheticAutoEq = "Preamp: -6.2 dB\n"
                                         "Filter 1: ON LSC Fc 105 Hz Gain 5.8 dB Q 0.70\n"
                                         "Filter 2: ON PK Fc 180 Hz Gain -2.6 dB Q 0.53\n"
                                         "Filter 3: ON PK Fc 1200 Hz Gain 1.9 dB Q 1.20\n"
                                         "Filter 4: ON PK Fc 2600 Hz Gain -3.4 dB Q 2.10\n"
                                         "Filter 5: ON PK Fc 3700 Hz Gain 4.2 dB Q 3.20\n"
                                         "Filter 6: ON PK Fc 5400 Hz Gain -5.1 dB Q 4.00\n"
                                         "Filter 7: ON PK Fc 7900 Hz Gain 3.3 dB Q 2.50\n"
                                         "Filter 8: ON PK Fc 11000 Hz Gain -2.2 dB Q 1.10\n"
                                         "Filter 9: ON PK Fc 16000 Hz Gain 1.5 dB Q 0.90\n"
                                         "Filter 10: ON HSC Fc 10000 Hz Gain -1.2 dB Q 0.70\n";

CorrectionCurve parseOrFail (const std::string& text)
{
    CorrectionCurve c;
    const auto r = eqtext::parse (text, c);
    if (! r.ok)
        std::cerr << "    parse error: " << r.error << "\n";
    REQUIRE (r.ok);
    return c;
}

CorrectionFilter filter (CorrectionFilterType type, float fc, float gainDb, float q, uint8_t channels = CorrectionFilter::kBoth)
{
    CorrectionFilter f;
    f.type = type;
    f.frequency = fc;
    f.gainDb = gainDb;
    f.q = q;
    f.channels = channels;
    return f;
}

/** Independent reference: the RBJ Audio-EQ-Cookbook biquads (direct form
    coefficients in double), evaluated at z = e^jw. Shares no code with Svf.h. */
double rbjMagnitudeDb (const CorrectionFilter& f, double freqHz, double fs)
{
    const double w0 = 2.0 * kPi * std::min (static_cast<double> (f.frequency), 0.49 * fs) / fs;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double A = std::pow (10.0, f.gainDb / 40.0);
    const double alpha = sw / (2.0 * f.q);
    const double sA = 2.0 * std::sqrt (A) * alpha;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (f.type)
    {
        case CorrectionFilterType::Peak:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case CorrectionFilterType::LowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sA);
            a0 = (A + 1) + (A - 1) * cw + sA; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sA;
            break;
        case CorrectionFilterType::HighShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sA);
            a0 = (A + 1) - (A - 1) * cw + sA; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sA;
            break;
        case CorrectionFilterType::LowPass:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case CorrectionFilterType::HighPass:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case CorrectionFilterType::BandPass:
            b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case CorrectionFilterType::Notch:
            b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case CorrectionFilterType::AllPass:
            b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
    }
    const std::complex<double> z1 = std::polar (1.0, -2.0 * kPi * freqHz / fs), z2 = z1 * z1;
    return 20.0 * std::log10 (std::abs ((b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2)));
}

double rbjCurveDb (const CorrectionCurve& c, int channel, double freqHz, double fs)
{
    double db = c.gainDb[static_cast<size_t> (channel)];
    for (int i = 0; i < c.numFilters; ++i)
    {
        const auto& f = c.filters[static_cast<size_t> (i)];
        if ((f.channels & (channel == 0 ? CorrectionFilter::kLeft : CorrectionFilter::kRight)) != 0)
            db += rbjMagnitudeDb (f, freqHz, fs);
    }
    return db;
}

/** |DFT| (dB) of x at freqHz (Goertzel-style recurrence, any frequency). */
double dftMagnitudeDb (const std::vector<float>& x, double freqHz, double fs)
{
    const double w = 2.0 * kPi * freqHz / fs, coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x)
    {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (re * re + im * im)));
}

std::unique_ptr<DeviceCorrection> makeCorrection (const DeviceCorrectionSettings& s, double fs = kFs, int maxBlock = 512)
{
    auto dc = std::make_unique<DeviceCorrection>();
    dc->setSettingsNow (s);
    dc->prepare ({ fs, maxBlock, 2 });
    return dc;
}

DeviceCorrectionSettings settingsFor (const CorrectionCurve& c)
{
    DeviceCorrectionSettings s;
    s.curve = c;
    return s;
}

/** Impulse responses (left, right) of a correction, `length` samples. */
std::array<std::vector<float>, 2> impulseResponses (DeviceCorrection& dc, int length, int blockSize = 512)
{
    Planar buf (2, length);
    buf.ch[0][0] = buf.ch[1][0] = 1.0f;
    processInBlocks (dc, buf, blockSize);
    return { buf.ch[0], buf.ch[1] };
}
} // namespace

// =============================================================================
// Import
// =============================================================================
TEST_CASE ("DeviceCorrection (E15): an AutoEQ ParametricEQ.txt imports filter for filter")
{
    CorrectionCurve c;
    const auto r = eqtext::parse (kSyntheticAutoEq, c);
    REQUIRE (r.ok);
    CHECK (r.warnings.empty());
    CHECK (c.numFilters == 10);
    CHECK (c.gainDb[0] == -6.2f && c.gainDb[1] == -6.2f);
    CHECK (c.filters[0] == filter (CorrectionFilterType::LowShelf, 105.0f, 5.8f, 0.70f));
    CHECK (c.filters[5] == filter (CorrectionFilterType::Peak, 5400.0f, -5.1f, 4.0f));
    CHECK (c.filters[9] == filter (CorrectionFilterType::HighShelf, 10000.0f, -1.2f, 0.70f));
    CHECK (c.countFor (0) == 10 && c.countFor (1) == 10);

    // Round trip through the persisted form is exact.
    CorrectionCurve back;
    REQUIRE (eqtext::parse (eqtext::format (c), back).ok);
    CHECK (back == c);
}

TEST_CASE ("DeviceCorrection (E15): Equalizer APO syntax - channels, per-channel preamp, BW, slopes, OFF, comments, CRLF, BOM")
{
    const std::string text = "\xEF\xBB\xBF# Peace export\r\n"
                             "Device: all\r\n"
                             "Preamp: -3 dB\r\n"
                             "Channel: L\r\n"
                             "Preamp: -1,5 dB\r\n"
                             "Filter: ON PK Fc 1000 Hz Gain 3 dB BW Oct 1.0\r\n"
                             "Channel: R\r\n"
                             "Filter 2: ON PEQ Fc 2000Hz Gain -2dB Q 2\r\n"
                             "Filter 3: OFF PK Fc 3000 Hz Gain 9 dB Q 1\r\n"
                             "Channel: all\r\n"
                             "Filter 4: ON LSC 12 dB Fc 80 Hz Gain 4 dB\r\n"
                             "Filter 5: ON HPQ Fc 20 Hz Q 0.5\r\n"
                             "Filter 6: ON LP Fc 18000 Hz\r\n"
                             "Filter 7: ON NO Fc 6000 Hz Q 8\r\n"
                             "Filter 8: ON AP Fc 500 Hz Q 0.7\r\n"
                             "Filter 9: ON BP Fc 700 Hz BW Oct 2\r\n"
                             "   # indented comment\r\n";
    CorrectionCurve c;
    const auto r = eqtext::parse (text, c);
    if (! r.ok)
        std::cerr << "    " << r.error << "\n";
    REQUIRE (r.ok);
    CHECK (r.warnings.empty());
    CHECK_NEAR (c.gainDb[0], -4.5, 1e-6);
    CHECK_NEAR (c.gainDb[1], -3.0, 1e-6);
    REQUIRE (c.numFilters == 8);
    CHECK (c.filters[0].channels == CorrectionFilter::kLeft);
    CHECK_NEAR (c.filters[0].q, std::sqrt (2.0) / 1.0, 1e-6); // BW 1 oct = Q 1.414
    CHECK (c.filters[1].channels == CorrectionFilter::kRight);
    CHECK (c.filters[1] == filter (CorrectionFilterType::Peak, 2000.0f, -2.0f, 2.0f, CorrectionFilter::kRight));
    // RBJ shelf slope S = 1 (12 dB/oct) is Q = 1/sqrt(2) at any gain.
    CHECK (c.filters[2].type == CorrectionFilterType::LowShelf);
    CHECK_NEAR (c.filters[2].q, 0.70710678, 1e-6);
    CHECK (c.filters[3] == filter (CorrectionFilterType::HighPass, 20.0f, 0.0f, 0.5f));
    CHECK_NEAR (c.filters[4].q, 0.70710678, 1e-6);
    CHECK (c.filters[5].type == CorrectionFilterType::Notch);
    CHECK (c.filters[6].type == CorrectionFilterType::AllPass);
    CHECK_NEAR (c.filters[7].q, std::sqrt (4.0) / 3.0, 1e-6);
    CHECK (c.countFor (0) == 7 && c.countFor (1) == 7);

    CorrectionCurve back;
    REQUIRE (eqtext::parse (eqtext::format (c), back).ok);
    CHECK (back == c);

    // A 6 dB/oct slope is gentler: S = 0.5 -> Q from the RBJ relation.
    REQUIRE (eqtext::parse ("Filter: ON HSC 6 dB Fc 8000 Hz Gain 6 dB", c).ok);
    const double A = std::pow (10.0, 6.0 / 40.0);
    CHECK_NEAR (c.filters[0].q, 1.0 / std::sqrt ((A + 1.0 / A) * (1.0 / 0.5 - 1.0) + 2.0), 1e-6);
}

TEST_CASE ("DeviceCorrection (E15): unsupported commands are refused with a clear message, the curve untouched")
{
    const auto base = parseOrFail (kSyntheticAutoEq);
    const std::array<std::pair<const char*, const char*>, 13> cases { {
        { "GraphicEQ: 20 -3; 1000 0; 20000 2", "GraphicEQ" },
        { "Include: other.txt", "Include" },
        { "Convolution: room.wav", "Convolution" },
        { "Stage: pre-mix", "Stage" },
        { "Delay: 10 ms", "not supported" },
        { "Filter: ON LS 6dB Fc 100 Hz Gain 3 dB", "corner-frequency" },
        { "Filter: ON PK Fc 1000 Hz Gain 3 dB", "needs a Q" },
        { "Filter: ON XYZ Fc 1000 Hz", "unsupported filter type" },
        { "Filter: ON PK Fc 0 Hz Gain 3 dB Q 1", "Fc" },
        { "Filter: ON PK Fc 1000 Hz Gain 40 dB Q 1", "30 dB" },
        { "Frobnicate: 3", "unknown command" },
        { "just some text", "not a ParametricEQ line" },
        { "Preamp: -30 dB", "add up" }, // each line within +-30 dB, together -31 dB
    } };
    for (const auto& [text, expected] : cases)
    {
        auto c = base;
        const auto r = eqtext::parse (std::string ("Preamp: -1 dB\n") + text + "\n", c);
        CHECK (! r.ok);
        CHECK (r.error.find ("line 2") == 0);
        if (r.error.find (expected) == std::string::npos)
            std::cerr << "    unexpected message for \"" << text << "\": " << r.error << "\n";
        CHECK (r.error.find (expected) != std::string::npos);
        CHECK (c == base);
    }

    // 17 filters on one channel.
    std::string many;
    for (int i = 0; i < 17; ++i)
        many += "Filter: ON PK Fc " + std::to_string (100 + 100 * i) + " Hz Gain 1 dB Q 1\n";
    CorrectionCurve c;
    CHECK (! eqtext::parse (many, c).ok);
    // ... but 16 per channel on two channels is fine.
    std::string perChannel = "Channel: L\n";
    for (int i = 0; i < 16; ++i)
        perChannel += "Filter: ON PK Fc " + std::to_string (100 + 100 * i) + " Hz Gain 1 dB Q 1\n";
    perChannel += "Channel: R\n";
    for (int i = 0; i < 16; ++i)
        perChannel += "Filter: ON PK Fc " + std::to_string (150 + 100 * i) + " Hz Gain -1 dB Q 1\n";
    CHECK (eqtext::parse (perChannel, c).ok);
    CHECK (c.numFilters == 32);

    CHECK (! eqtext::parse ("# only a comment\n\n", c).ok);
}

TEST_CASE ("DeviceCorrection (E15): ignored lines and assumptions are reported as warnings")
{
    CorrectionCurve c;
    const auto r = eqtext::parse ("Device: Speakers Realtek\n"
                                  "Channel: C LFE\n"
                                  "Filter: ON PK Fc 60 Hz Gain 6 dB Q 1\n"
                                  "Channel: L R C\n"
                                  "Filter: ON LSC Fc 100 Hz Gain 2 dB\n",
                                  c);
    REQUIRE (r.ok);
    CHECK (r.warnings.size() == 4); // device, dropped C/LFE filter, C in a mixed selection, shelf without Q
    CHECK (c.numFilters == 1);
    CHECK (c.filters[0].channels == CorrectionFilter::kBoth);
    CHECK_NEAR (c.filters[0].q, 0.70710678, 1e-6);
}

// =============================================================================
// Response
// =============================================================================
TEST_CASE ("DeviceCorrection (E15): rendered response matches an independent RBJ reference within 0.1 dB, 20 Hz - 20 kHz")
{
    // The synthetic AutoEQ curve plus one of every other filter type on a
    // per-channel curve, at the common device rates.
    const auto autoEq = parseOrFail (kSyntheticAutoEq);
    const auto perChannel = parseOrFail ("Channel: L\nPreamp: -2 dB\nFilter: ON PK Fc 40 Hz Gain 4 dB Q 2\nFilter: ON NO Fc 9000 Hz Q 6\n"
                                         "Channel: R\nFilter: ON HPQ Fc 25 Hz Q 0.6\nFilter: ON LPQ Fc 17000 Hz Q 0.8\n"
                                         "Filter: ON AP Fc 800 Hz Q 1\nFilter: ON BP Fc 3000 Hz Q 0.3\n"
                                         "Channel: all\nFilter: ON HSC 9 dB Fc 6000 Hz Gain 3 dB\n");
    for (const auto* curve : { &autoEq, &perChannel })
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            auto dc = makeCorrection (settingsFor (*curve), fs);
            const double preamp = dc->getPreampDb();
            const auto ir = impulseResponses (*dc, static_cast<int> (fs)); // 1 s: every section has decayed below -150 dB
            double worst = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i <= 240; ++i) // 24 points per octave
                {
                    const double f = 20.0 * std::pow (1000.0, i / 240.0);
                    const double rendered = dftMagnitudeDb (ir[static_cast<size_t> (ch)], f, fs) - preamp;
                    const double reference = rbjCurveDb (*curve, ch, f, fs);
                    worst = std::max (worst, std::abs (rendered - reference));
                    // The analytic response the predictor uses agrees as well.
                    CHECK_NEAR (curve->responseDb (ch, f, fs), reference, 0.01);
                }
            std::cout << "    fs " << fs << ": worst |rendered - RBJ| " << worst << " dB\n";
            CHECK_LE (worst, 0.1);
        }
}

// =============================================================================
// Headroom (E11)
// =============================================================================
TEST_CASE ("Headroom predictor (E11): finds the maximum boost, including a narrow peak between grid points")
{
    using namespace headroom;
    CorrectionCurve c;
    c.add (filter (CorrectionFilterType::Peak, 1000.0f, 6.0f, 1.0f));
    const auto resp = [&c] (double f) { return c.responseDb (0, f, kFs); };
    auto p = predictMaxBoost (resp);
    CHECK_NEAR (p.maxBoostDb, 6.0, 1e-3);
    CHECK_NEAR (p.atHz, 1000.0, 5.0);
    CHECK_NEAR (preampDb (p), -6.0, 1e-3);
    CHECK_NEAR (preampDb (p, 1.0), -5.0, 1e-3); // E11's allowance

    // Q 12 at a quarter-grid offset: the 1/12-octave grid alone reads it low.
    const double between = 3000.0 * std::exp2 (1.0 / 48.0);
    CorrectionCurve narrow;
    narrow.add (filter (CorrectionFilterType::Peak, static_cast<float> (between), 8.0f, 12.0f));
    narrow.add (filter (CorrectionFilterType::LowShelf, 100.0f, 7.5f, 0.7f)); // a broad rival just below it
    p = predictMaxBoost ([&narrow] (double f) { return narrow.responseDb (0, f, kFs); });
    CHECK_NEAR (p.maxBoostDb, narrow.responseDb (0, between, kFs), 0.01);
    CHECK_GE (p.maxBoostDb, 8.0);
    CHECK_NEAR (p.atHz, between, between * 0.002);

    // Only cuts: no preamp. A file preamp is part of the response.
    CorrectionCurve cuts;
    cuts.add (filter (CorrectionFilterType::Peak, 3000.0f, -6.0f, 2.0f));
    CHECK (preampDb (predictMaxBoost ([&cuts] (double f) { return cuts.responseDb (0, f, kFs); })) == 0.0f);
    const auto autoEq = parseOrFail (kSyntheticAutoEq);
    auto dc = makeCorrection (settingsFor (autoEq));
    std::cout << "    synthetic AutoEQ curve: max boost " << dc->getPrediction().maxBoostDb << " dB at " << dc->getPrediction().atHz
              << " Hz (file preamp -6.2 dB included) -> automatic preamp " << dc->getPreampDb() << " dB\n";
    CHECK_LE (dc->getPreampDb(), 0.0f);

    // Programme weighting weighs a 10 kHz boost 10 dB lower than a 500 Hz one.
    CHECK_NEAR (programmeEnvelopeDb (500.0), 0.0, 1e-9);
    CHECK_NEAR (programmeEnvelopeDb (8000.0), -9.0, 1e-9);
    CHECK_NEAR (programmeEnvelopeDb (20.0), -6.0, 1e-9);
    CorrectionCurve treble;
    treble.add (filter (CorrectionFilterType::HighShelf, 8000.0f, 6.0f, 0.7f));
    const auto trebleResp = [&treble] (double f) { return treble.responseDb (0, f, kFs); };
    CHECK_GE (predictMaxBoost (trebleResp).maxBoostDb, 5.9);
    CHECK_LE (predictMaxBoost (trebleResp, Weighting::Programme).maxBoostDb, 1.0);
}

TEST_CASE ("DeviceCorrection (E11 / E15): after the automatic preamp no sine grows and 0 dBFS pink stays below 0 dBFS")
{
    auto boosty = parseOrFail (kSyntheticAutoEq);
    boosty.gainDb = { 0.0f, 0.0f }; // drop the file's preamp: the automatic one has to do all the work
    auto dc = makeCorrection (settingsFor (boosty));
    CHECK_LE (dc->getPreampDb(), -5.0f);

    // Steady sines at the predicted maximum and around it, within the
    // predictor's 20 Hz - 20 kHz (here the maximum is the low shelf's, at 20 Hz).
    const double fPeak = dc->getPrediction().atHz;
    for (double f : { fPeak, fPeak * 1.1, fPeak * 2.0, 1000.0, 3700.0, 7900.0, 16000.0 })
    {
        const double analytic = boosty.responseDb (0, f, kFs) + dc->getPreampDb();
        const double measured = measureGainDb (*dc, f, kFs, 2, 0.5f);
        std::cout << "    " << f << " Hz: analytic " << analytic << " measured " << measured << "\n";
        CHECK_LE (analytic, 1e-6);
        CHECK_LE (measured, 0.005);
    }

    // Pink noise normalised to a 0 dBFS sample peak.
    const int n = static_cast<int> (kFs * 10.0);
    auto pink = pinkNoise (n, 0.2f, 99);
    const double inPeak = peakAbs (pink.data(), n);
    Planar buf (2, n);
    for (int i = 0; i < n; ++i)
        buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = static_cast<float> (pink[static_cast<size_t> (i)] / inPeak);
    dc->reset();
    processInBlocks (*dc, buf, 512);
    const double outPeakDb = toDb (std::max (peakAbs (buf.ch[0].data(), n), peakAbs (buf.ch[1].data(), n)));
    std::cout << "    0 dBFS-peak pink after the correction: " << outPeakDb << " dBFS (preamp " << dc->getPreampDb() << " dB)\n";
    CHECK_LE (outPeakDb, -0.1);
}

// =============================================================================
// Transitions, bypass, robustness
// =============================================================================
TEST_CASE ("DeviceCorrection (E15): a curve change crossfades click-free over 20 ms; a change mid-fade waits; newest wins")
{
    const auto a = parseOrFail (kSyntheticAutoEq);
    const auto b = parseOrFail ("Filter: ON LSC Fc 60 Hz Gain -4 dB Q 0.7\nFilter: ON PK Fc 1000 Hz Gain 5 dB Q 0.8\nFilter: ON PK Fc 6000 Hz Gain 6 dB Q 5\n");
    const int n = static_cast<int> (kFs);
    const int swapAt = 94 * 256; // a block boundary
    const int fade = static_cast<int> (std::lround (DeviceCorrection::kCrossfadeMs * 0.001 * kFs));

    // Programme: a 1 kHz tone plus a 60 Hz tone (the low shelf's transient is the slow part).
    Planar in (2, n);
    for (int i = 0; i < n; ++i)
        in.ch[0][static_cast<size_t> (i)] = in.ch[1][static_cast<size_t> (i)]
            = static_cast<float> (0.25 * std::sin (kTwoPi * 1000.0 * i / kFs) + 0.25 * std::sin (kTwoPi * 60.0 * i / kFs + 0.4));

    // Steady outputs of A and B alone, for the ideal equal-gain crossfade.
    auto outA = in, outB = in;
    processInBlocks (*makeCorrection (settingsFor (a)), outA, 256);
    processInBlocks (*makeCorrection (settingsFor (b)), outB, 256);

    auto dc = makeCorrection (settingsFor (a));
    auto out = in;
    const auto before = dc->getCompletedTransitions();
    for (int pos = 0; pos < n; pos += 256)
    {
        if (pos == swapAt)
            REQUIRE (dc->setSettings (settingsFor (b)));
        dc->process (out.block (pos, std::min (256, n - pos)));
    }
    CHECK (dc->getCompletedTransitions() == before + 1);

    double worstDeviation = 0.0, worstStep = 0.0, steadyStep = 0.0;
    for (int i = 1; i < n; ++i)
    {
        const auto k = static_cast<size_t> (i);
        const double g = i < swapAt ? 0.0 : i >= swapAt + fade ? 1.0 : 0.5 - 0.5 * std::cos (kPi * (i - swapAt + 0.5) / fade);
        const double ideal = (1.0 - g) * outA.ch[0][k] + g * outB.ch[0][k];
        worstDeviation = std::max (worstDeviation, std::abs (out.ch[0][k] - ideal));
        const double step = std::abs (out.ch[0][k] - out.ch[0][k - 1]);
        auto& bound = i >= swapAt && i < swapAt + fade ? worstStep : steadyStep;
        bound = std::max (bound, step);
    }
    std::cout << "    swap: max deviation from the ideal crossfade " << toDb (worstDeviation) << " dBFS; max step in fade "
              << worstStep << " vs steady " << steadyStep << "\n";
    CHECK_LE (toDb (worstDeviation), -40.0);
    CHECK_LE (worstStep, 1.05 * steadyStep);
    // After the fade the output is B's, sample for sample (states carried over).
    double tail = 0.0;
    for (int i = swapAt + fade + 4800; i < n; ++i)
        tail = std::max (tail, static_cast<double> (std::abs (out.ch[1][static_cast<size_t> (i)] - outB.ch[1][static_cast<size_t> (i)])));
    CHECK_LE (tail, 1e-4);

    // Three changes inside one fade: the first runs, the last one follows it, the middle one is skipped.
    auto dc2 = makeCorrection (settingsFor (a));
    Planar blk (2, 256);
    const auto start = dc2->getCompletedTransitions();
    REQUIRE (dc2->setSettings (settingsFor (b)));
    dc2->process (blk.block());
    REQUIRE (dc2->setSettings (settingsFor (a)));
    DeviceCorrectionSettings off;
    off.enabled = false;
    REQUIRE (dc2->setSettings (off));
    for (int i = 0; i < 40; ++i)
        dc2->process (blk.block());
    CHECK (dc2->getCompletedTransitions() == start + 2);
    // ... and it ends on "off": bit-exact passthrough.
    Planar probe (2, 256);
    for (int i = 0; i < 256; ++i)
        probe.ch[0][static_cast<size_t> (i)] = probe.ch[1][static_cast<size_t> (i)] = static_cast<float> (std::sin (i * 0.1));
    const auto ref = probe;
    dc2->process (probe.block());
    CHECK (probe.ch == ref.ch);
}

TEST_CASE ("DeviceCorrection (E15): off is a bit-exact passthrough; compare keeps the broadband gain without the filters")
{
    const auto c = parseOrFail (kSyntheticAutoEq);
    Planar sig (2, 4096);
    for (int i = 0; i < 4096; ++i)
    {
        sig.ch[0][static_cast<size_t> (i)] = static_cast<float> (0.3 * std::sin (i * 0.05));
        sig.ch[1][static_cast<size_t> (i)] = static_cast<float> (0.2 * std::sin (i * 0.21));
    }

    // A fresh correction and a disabled one are free and exact.
    DeviceCorrection fresh;
    auto s = settingsFor (c);
    s.enabled = false;
    auto off = makeCorrection (s);
    for (auto* dc : { &fresh, off.get() })
    {
        auto buf = sig;
        dc->process (buf.block());
        CHECK (buf.ch == sig.ch);
    }
    CHECK (off->getPreampDb() == 0.0f);

    // Compare: flat at the correction's broadband gain (file preamp + automatic preamp).
    s.enabled = true;
    s.compare = true;
    auto cmp = makeCorrection (s);
    const double expected = c.gainDb[0] + cmp->getPreampDb();
    for (double f : { 50.0, 1000.0, 5400.0, 12000.0 })
        CHECK_NEAR (measureGainDb (*cmp, f, kFs), expected, 0.01);
    auto on = makeCorrection (settingsFor (c));
    CHECK (on->getPreampDb() == cmp->getPreampDb());
}

TEST_CASE ("DeviceCorrection (E15): process is allocation-free, recovers from NaN / Inf and does not depend on the block size")
{
    const auto c = parseOrFail (kSyntheticAutoEq);
    const int n = 20000;
    Planar sig (2, n);
    auto noise = whiteNoise (n, 0.3f, 5);
    for (int i = 0; i < n; ++i)
        sig.ch[0][static_cast<size_t> (i)] = sig.ch[1][static_cast<size_t> (i)] = noise[static_cast<size_t> (i)];

    std::vector<Planar> outs;
    for (int block : { 1, 7, 64, 512, 4096 })
    {
        auto dc = makeCorrection (settingsFor (parseOrFail ("Filter: ON PK Fc 300 Hz Gain -3 dB Q 1\n")), kFs, 4096);
        REQUIRE (dc->setSettings (settingsFor (c))); // a crossfade at the first block
        auto buf = sig;
        ScopedNoDenormals noDenormals;
        AllocationGuard guard;
        processInBlocks (*dc, buf, block);
        CHECK (guard.allocations() == 0);
        outs.push_back (std::move (buf));
    }
    for (size_t i = 1; i < outs.size(); ++i)
        CHECK (outs[i].ch == outs[0].ch);

    auto dc = makeCorrection (settingsFor (c));
    Planar bad (2, 512);
    bad.ch[0][100] = std::numeric_limits<float>::quiet_NaN();
    bad.ch[1][200] = std::numeric_limits<float>::infinity();
    dc->process (bad.block());
    auto good = sig;
    dc->process (good.block (0, 512));
    dc->process (good.block (512, 512));
    bool finite = true;
    for (const auto& ch : good.ch)
        for (int i = 512; i < 1024; ++i)
            finite = finite && std::isfinite (ch[static_cast<size_t> (i)]);
    CHECK (finite);
}

TEST_CASE ("MixEngine (E15): the device correction runs on the sum before the master limiter and survives reconfiguration")
{
    const std::vector<StripConfig> layout { { "Music", 2, 0.0f, false, StripConfig::kNoSyncGroup } };
    // A +6 dB bell at 3 kHz: its automatic preamp lowers a 100 Hz tone by 6 dB.
    const auto curve = parseOrFail ("Filter: ON PK Fc 3000 Hz Gain 6 dB Q 1\n");

    const auto run = [] (MixEngine& m, float amplitude, double freq, int blocks)
    {
        Planar in (2, 256), out (2, 256);
        const AudioBlock ib = in.block();
        const AudioBlock* inputs[] = { &ib };
        std::vector<float> tail;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 256; ++i)
                in.ch[0][static_cast<size_t> (i)] = in.ch[1][static_cast<size_t> (i)]
                    = amplitude * static_cast<float> (std::sin (kTwoPi * freq * (b * 256 + i) / kFs));
            m.process (inputs, out.block());
            if (b >= blocks / 2)
                tail.insert (tail.end(), out.ch[0].begin(), out.ch[0].end());
        }
        return tail;
    };

    MixEngine plain, corrected;
    plain.configure (layout, kFs, 256);
    corrected.configure (layout, kFs, 256);
    DeviceCorrectionSettings s;
    s.curve = curve;
    corrected.getDeviceCorrection().setSettingsNow (s);
    CHECK_NEAR (corrected.getDeviceCorrection().getPreampDb(), -6.0, 0.01);

    const auto a = run (plain, 0.05f, 100.0, 200);
    const auto b = run (corrected, 0.05f, 100.0, 200);
    const int m = static_cast<int> (a.size());
    const double delta = toDb (toneAmplitude (b.data(), m, 100.0, kFs) / toneAmplitude (a.data(), m, 100.0, kFs));
    CHECK_NEAR (delta, curve.responseDb (0, 100.0, kFs) - 6.0, 0.05);

    // Before the limiter: a full-scale 3 kHz tone, +6 dB by the curve and -6 dB by the preamp, still meets the ceiling.
    const auto loud = run (corrected, 1.0f, 3000.0, 200);
    CHECK_LE (peakAbs (loud.data(), static_cast<int> (loud.size())), dbToGain (-1.0f) + 1e-4);

    // configure() keeps it (at the new rate); configureFrom() copies it; presets never see it.
    corrected.configure (layout, 96000.0, 256);
    CHECK (corrected.getDeviceCorrection().getSettings() == s);
    MixEngine next;
    next.configureFrom (corrected, layout, kFs, 256);
    CHECK (next.getDeviceCorrection().getSettings() == s);
    CHECK_NEAR (next.getDeviceCorrection().getPreampDb(), -6.0, 0.01);
}
