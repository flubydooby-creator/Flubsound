// Spatial metrics (docs/11 E60 stage 2; flub/analysis/SpatialMetrics.h): IACC
// (early / late, per octave), ITD, DRR, 1/3-octave levels and the
// diffuse-field deviation of binaural impulse responses. Meta-validation on
// synthetic responses with known answers, the renderer's own HRIRs against
// the virtualiser, today's weak values of the parametric renderer (pinned:
// the E28 renderer work is measured against them) and a synthetic pinna
// notch moving the diffuse-field deviation at its band.
#include "TestFramework.h"

#include "flub/analysis/SpatialMetrics.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace flub;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kLength = 12000; // 250 ms: every response of the renderer ends well inside

BinauralIr impulsePair (int at, float gainL, float gainR, int delayR = 0, int length = kLength)
{
    BinauralIr ir;
    ir.left.assign (static_cast<size_t> (length), 0.0f);
    ir.right.assign (static_cast<size_t> (length), 0.0f);
    ir.left[static_cast<size_t> (at)] = gainL;
    ir.right[static_cast<size_t> (at + delayR)] = gainR;
    return ir;
}

size_t bandIndex (const std::vector<double>& bands, double hz)
{
    return static_cast<size_t> (std::find (bands.begin(), bands.end(), hz) - bands.begin());
}

/** Both ears' power sum per 1/3-octave band (dB). */
double bothEarsDb (const BinauralIrMetrics& m, size_t b)
{
    return 10.0 * std::log10 (std::pow (10.0, 0.1 * m.leftDb[b]) + std::pow (10.0, 0.1 * m.rightDb[b]));
}

BinauralIrMetrics speaker (int channel, float room)
{
    VirtualizerParams p;
    p.roomAmount = room;
    return analyseBinauralIr (virtualizerResponse (p, 1u << channel, kFs, kLength), kFs);
}

/** RBJ peaking bell (a synthetic pinna notch when gainDb < 0) over x in place. */
void bell (std::vector<float>& x, double hz, double q, double gainDb)
{
    const double a = std::pow (10.0, gainDb / 40.0), w0 = kTwoPi * hz / kFs, alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha / a;
    const double b0 = (1.0 + alpha * a) / a0, b1 = -2.0 * std::cos (w0) / a0, b2 = (1.0 - alpha * a) / a0;
    const double a1 = b1, a2 = (1.0 - alpha / a) / a0;
    double z1 = 0.0, z2 = 0.0;
    for (auto& v : x)
    {
        const double y = b0 * v + z1;
        z1 = b1 * v - a1 * y + z2;
        z2 = b2 * v - a2 * y;
        v = static_cast<float> (y);
    }
}
} // namespace

TEST_CASE ("Spatial metrics: IACC, ITD, DRR and 1/3-octave levels read their known values on synthetic binaural impulses")
{
    // A dry centred source: both ears identical.
    auto centred = analyseBinauralIr (impulsePair (100, 0.5f, 0.5f), kFs);
    REQUIRE (centred.valid);
    CHECK_NEAR (centred.onsetSeconds, 100.0 / kFs, 1.0e-12);
    CHECK_NEAR (centred.iaccEarly, 1.0, 1.0e-9);
    CHECK_NEAR (centred.itdMs, 0.0, 1.0e-12);
    for (double v : centred.iaccEarlyBands)
        CHECK_NEAR (v, 1.0, 1.0e-6);
    CHECK (std::isnan (centred.iaccLate)); // no tail
    for (double v : centred.iaccLateBands)
        CHECK (std::isnan (v));
    CHECK (std::isinf (centred.drrDb) && centred.drrDb > 0.0);
    for (size_t b = 0; b < centred.bandsHz.size(); ++b)
    {
        CHECK_NEAR (centred.leftDb[b], -6.0206, 0.001); // 0.5: -6.02 dB in every band
        CHECK_NEAR (centred.rightDb[b], -6.0206, 0.001);
    }
    CHECK (centred.bandsHz.front() == 50.0 && centred.bandsHz.back() == 20000.0);

    // The far ear 6 dB down and 10 samples later: the lag is found, IACC 1
    // (a pure delay and gain), + = the right ear lags.
    auto lateral = analyseBinauralIr (impulsePair (100, 1.0f, 0.5f, 10), kFs);
    CHECK_NEAR (lateral.iaccEarly, 1.0, 1.0e-9);
    CHECK_NEAR (lateral.itdMs, 10.0 / 48.0, 1.0e-9);
    CHECK_NEAR (lateral.rightDb[10] - lateral.leftDb[10], -6.0206, 0.001);
    auto mirrored = analyseBinauralIr (impulsePair (110, 0.5f, 1.0f, -10), kFs);
    CHECK_NEAR (mirrored.itdMs, -10.0 / 48.0, 1.0e-9);
    // Outside the +-1 ms lag range the correlation of two impulses is 0.
    CHECK_NEAR (analyseBinauralIr (impulsePair (100, 1.0f, 1.0f, 60), kFs).iaccEarly, 0.0, 1.0e-9);

    // A reflection at 10 ms, -20 dB, in both ears: DRR +20 dB; IACC early
    // still 1 (the same in both ears); a reflection in one ear only lowers it
    // to 1 / sqrt(1.01).
    auto room = impulsePair (100, 1.0f, 1.0f);
    room.left[100 + 480] = room.right[100 + 480] = 0.1f;
    auto m = analyseBinauralIr (room, kFs);
    CHECK_NEAR (m.drrDb, 20.0, 0.001);
    CHECK_NEAR (m.iaccEarly, 1.0, 1.0e-9);
    room.right[100 + 480] = 0.0f;
    m = analyseBinauralIr (room, kFs);
    CHECK_NEAR (m.iaccEarly, 1.0 / std::sqrt (1.01), 1.0e-6);
    CHECK_NEAR (m.drrDb, 10.0 * std::log10 (2.0 / 0.01), 0.001);

    // A tail after 80 ms: late IACC 1 when both ears carry it, also with
    // opposite polarity (|correlation|), 0 when they are 4 ms apart.
    auto tail = impulsePair (100, 1.0f, 1.0f);
    tail.left[100 + 4800] = tail.right[100 + 4800] = 0.05f;
    CHECK_NEAR (analyseBinauralIr (tail, kFs).iaccLate, 1.0, 1.0e-9);
    tail.right[100 + 4800] = -0.05f;
    CHECK_NEAR (analyseBinauralIr (tail, kFs).iaccLate, 1.0, 1.0e-9);
    tail.right[100 + 4800] = 0.0f;
    tail.right[100 + 4800 + 192] = 0.05f; // 4 ms later: outside the lag range
    CHECK_NEAR (analyseBinauralIr (tail, kFs).iaccLate, 0.0, 1.0e-9);

    // Silence is not measured.
    BinauralIr silent;
    silent.left.assign (1000, 0.0f);
    silent.right.assign (1000, 0.0f);
    CHECK (! analyseBinauralIr (silent, kFs).valid);
    CHECK (directOnset (silent) == -1);

    // Bands: nominal labels, exact centres, the top band below fs / 2.
    CHECK_NEAR (thirdOctaveExactHz (1250.0), 1000.0 * std::pow (2.0, 1.0 / 3.0), 1.0e-9);
    CHECK_NEAR (thirdOctaveExactHz (31.5), 1000.0 * std::pow (2.0, -15.0 / 3.0), 1.0e-9);
    CHECK (thirdOctaveBands (44100.0).back() == 16000.0); // 20 kHz band's upper edge 22.4 kHz
    CHECK (thirdOctaveBands (32000.0).back() == 12500.0);
}

TEST_CASE ("Spatial metrics: parametricHrir is the virtualiser's own rendering of a speaker, sample for sample")
{
    VirtualizerParams p;
    p.roomAmount = 0.0f;
    for (int c : { 0, 1, 2, 4, 5, 6, 7 })
    {
        const auto module = virtualizerResponse (p, 1u << c, kFs, 512);
        std::vector<float> l, r;
        HeadphoneVirtualizer::parametricHrir (HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout::Surround71, c, p), p.headRadiusMm, kFs, 512, l, r);
        float diff = 0.0f;
        for (size_t i = 0; i < 512; ++i)
            diff = std::max ({ diff, std::abs (l[i] - module.left[i]), std::abs (r[i] - module.right[i]) });
        CHECK (diff <= 1.0e-7f);
    }
    // Any azimuth, wrapped: 270 is -90; the model is left / right symmetric.
    std::vector<float> a, b, c, d;
    HeadphoneVirtualizer::parametricHrir (270.0f, 87.5f, kFs, 256, a, b);
    HeadphoneVirtualizer::parametricHrir (90.0f, 87.5f, kFs, 256, c, d);
    CHECK (a == d && b == c);
}

TEST_CASE ("Spatial metrics: today's parametric renderer reproduces its weak values (7.1, defaults)")
{
    // docs/11 E28 "Why": the default room has no measurable effect (room
    // 0.15 vs 0 changes IACC by 0.01; FC IACC 1.00). Measured: FC early IACC
    // 1.000 dry, 0.995 at the default room; DRR +22.9 dB; nothing after 80 ms.
    const auto fcDry = speaker (2, 0.0f), fc = speaker (2, 0.15f);
    CHECK_NEAR (fcDry.iaccEarly, 1.0, 1.0e-6);
    CHECK_NEAR (fc.iaccEarly, 0.995, 0.003);
    CHECK (fcDry.iaccEarly - fc.iaccEarly <= 0.01);
    CHECK (std::isinf (fcDry.drrDb));
    CHECK_NEAR (fc.drrDb, 22.87, 0.2);
    for (double v : fc.iaccEarlyBands)
        CHECK (v >= 0.98); // every octave 0.985 .. 0.997
    const auto sl = speaker (6, 0.15f);
    CHECK_NEAR (sl.iaccEarly, 0.518, 0.01);   // a side speaker: 0.518 (0.527 dry)
    CHECK_NEAR (sl.itdMs, 0.604, 0.021);      // the right (far) ear lags by 29 samples
    CHECK_NEAR (sl.drrDb, 24.76, 0.2);
    for (int c : { 0, 2, 4, 6 })
        CHECK (std::isnan (speaker (c, 0.15f).iaccLate)); // no reverberant tail at all

    // Front / back: FL and BL differ by <= 0.35 dB at the near ear below 2 kHz
    // (the E28 figure: <= 0.3 dB) and only by the rear shelf above.
    const auto fl = speaker (0, 0.15f), bl = speaker (4, 0.15f);
    double worstLow = 0.0;
    for (size_t b = 0; b < fl.bandsHz.size() && fl.bandsHz[b] <= 2000.0; ++b)
        worstLow = std::max (worstLow, std::abs (fl.leftDb[b] - bl.leftDb[b]));
    CHECK_NEAR (worstLow, 0.35, 0.05);
    CHECK_NEAR (fl.leftDb[bandIndex (fl.bandsHz, 10000.0)] - bl.leftDb[bandIndex (bl.bandsHz, 10000.0)], 3.52, 0.1);

    // FC against the sides (both ears' power): up to 3.8 dB darker around
    // 2.5 kHz (the E28 figure "4.3 dB darker" was a single-frequency read).
    const auto sr = speaker (7, 0.15f);
    double darkest = 0.0, darkestHz = 0.0;
    for (size_t b = 0; b < fc.bandsHz.size(); ++b)
    {
        const double d = bothEarsDb (fc, b) - 0.5 * (bothEarsDb (sl, b) + bothEarsDb (sr, b));
        if (d < darkest)
        {
            darkest = d;
            darkestHz = fc.bandsHz[b];
        }
    }
    CHECK_NEAR (darkest, -3.82, 0.1);
    CHECK (darkestHz == 2500.0);

    // The same impulse on all seven speakers, dry: 28.06 dB peak-to-notch in
    // 4 - 8 kHz (the E28 figure: 28 dB; Done-when < 12 dB).
    VirtualizerParams dry;
    dry.roomAmount = 0.0f;
    const auto all = virtualizerResponse (dry, 0xF7u, kFs, kLength);
    CHECK_NEAR (peakToNotchDb (all.left, kFs, 4000.0, 8000.0), 28.06, 0.1);
    CHECK_NEAR (peakToNotchDb (all.right, kFs, 4000.0, 8000.0), 28.06, 0.1);

    // Diffuse field of the seven speakers at the default room: 3.5 dB range,
    // +0.9 dB at 1.6 kHz and -2.6 dB at 16 kHz re its mean (no diffuse-field
    // equalisation).
    std::vector<BinauralIr> irs;
    VirtualizerParams p;
    for (int c : { 0, 1, 2, 4, 5, 6, 7 })
        irs.push_back (virtualizerResponse (p, 1u << c, kFs, kLength));
    const auto df = diffuseField (irs, kFs);
    CHECK (df.bandsHz.front() == 100.0 && df.bandsHz.back() == 16000.0);
    CHECK_NEAR (df.rangeDb, 3.52, 0.1);
    CHECK_NEAR (df.rmsDeviationDb, 0.93, 0.05);
    CHECK_NEAR (df.deviationDb[bandIndex (df.bandsHz, 16000.0)], -2.64, 0.1);
}

TEST_CASE ("Spatial metrics: a synthetic pinna notch moves the diffuse-field deviation at its band, and a shift of the notch follows it")
{
    // The renderer's own HRIRs at 24 azimuths (15 degree steps), without and
    // with a -15 dB, Q 4 notch at 8 kHz, then with the notch moved to 10 kHz.
    std::vector<BinauralIr> plain, notch8, notch10;
    for (int a = 0; a < 360; a += 15)
    {
        BinauralIr ir;
        HeadphoneVirtualizer::parametricHrir (static_cast<float> (a), 87.5f, kFs, 1024, ir.left, ir.right);
        plain.push_back (ir);
        for (auto* set : { &notch8, &notch10 })
        {
            auto n = ir;
            for (auto* ear : { &n.left, &n.right })
                bell (*ear, set == &notch8 ? 8000.0 : 10000.0, 4.0, -15.0);
            set->push_back (n);
        }
    }
    const auto base = diffuseField (plain, kFs), d8 = diffuseField (notch8, kFs), d10 = diffuseField (notch10, kFs);
    CHECK_NEAR (base.rangeDb, 2.02, 0.05); // today's 24-direction diffuse field
    // Level change per band (the deviation is re each curve's own mean).
    const auto change = [&base] (const DiffuseField& d, size_t b) { return d.levelDb[b] - base.levelDb[b]; };
    const size_t b8 = bandIndex (base.bandsHz, 8000.0), b10 = bandIndex (base.bandsHz, 10000.0);
    size_t min8 = 0, min10 = 0;
    for (size_t b = 0; b < base.bandsHz.size(); ++b)
    {
        min8 = change (d8, b) < change (d8, min8) ? b : min8;
        min10 = change (d10, b) < change (d10, min10) ? b : min10;
    }
    CHECK (min8 == b8);
    CHECK (min10 == b10);
    CHECK (change (d8, b8) < -3.0);
    CHECK (change (d10, b10) < -3.0);
    // Moving the notch up restores the 8 kHz band and takes the 10 kHz one.
    CHECK (change (d10, b8) - change (d8, b8) > 2.0);
    CHECK (change (d10, b10) - change (d8, b10) < -2.0);
    // Measured: -10.2 dB at 8 kHz (-3.5 / -2.9 dB in the bands either side),
    // then -9.5 dB at 10 kHz; two octaves below the notch, nothing moves.
    CHECK_NEAR (change (d8, b8), -10.19, 0.1);
    CHECK_NEAR (change (d10, b10), -9.49, 0.1);
    for (size_t b = 0; b < base.bandsHz.size() && base.bandsHz[b] <= 2000.0; ++b)
    {
        CHECK (std::abs (change (d8, b)) < 0.1);
        CHECK (std::abs (change (d10, b)) < 0.1);
    }
    // The same notch in both ears leaves the interaural coherence of the
    // octaves below it as it was (within 0.005 at every azimuth; the
    // broadband IACC moves, as the notch re-weights the spectrum); in one ear
    // only it lowers the 8 kHz octave's IACC.
    for (size_t k = 0; k < plain.size(); k += 3)
        for (size_t o = 0; o < 5; ++o) // 125 Hz .. 2 kHz
            CHECK_NEAR (analyseBinauralIr (notch8[k], kFs).iaccEarlyBands[o], analyseBinauralIr (plain[k], kFs).iaccEarlyBands[o], 0.005);
    auto oneEar = plain[0];
    bell (oneEar.left, 8000.0, 4.0, -15.0);
    CHECK (analyseBinauralIr (oneEar, kFs).iaccEarlyBands[6] < analyseBinauralIr (plain[0], kFs).iaccEarlyBands[6] - 0.05);
}

TEST_CASE ("Spatial metrics: splitImpulses separates responses at least 100 ms apart and keeps each one's reflections")
{
    // FC, SR and BL through the virtualiser at full room (reflections up to
    // 19 ms at -6 dB) at 0.05, 0.30 and 0.55 s.
    VirtualizerParams p;
    p.roomAmount = 1.0f;
    const int n = static_cast<int> (0.8 * kFs);
    std::vector<float> l (static_cast<size_t> (n), 0.0f), r (l.size(), 0.0f);
    const std::array<int, 3> channels { 2, 7, 4 };
    std::vector<BinauralIrMetrics> direct;
    for (size_t k = 0; k < channels.size(); ++k)
    {
        const auto ir = virtualizerResponse (p, 1u << channels[k], kFs, kLength);
        direct.push_back (analyseBinauralIr (ir, kFs));
        const auto at = static_cast<size_t> ((0.05 + 0.25 * static_cast<double> (k)) * kFs);
        for (size_t i = 0; i < ir.left.size(); ++i)
        {
            l[at + i] += ir.left[i];
            r[at + i] += ir.right[i];
        }
    }
    std::vector<double> onsets;
    const auto irs = splitImpulses (l, r, kFs, 0.1, &onsets);
    REQUIRE (irs.size() == 3);
    for (size_t k = 0; k < irs.size(); ++k)
    {
        CHECK_NEAR (onsets[k], 0.05 + 0.25 * static_cast<double> (k) + direct[k].onsetSeconds, 1.0e-9);
        const auto m = analyseBinauralIr (irs[k], kFs);
        CHECK_NEAR (m.iaccEarly, direct[k].iaccEarly, 1.0e-6);
        CHECK_NEAR (m.drrDb, direct[k].drrDb, 0.01);
        CHECK_NEAR (m.itdMs, direct[k].itdMs, 1.0e-9);
    }
    CHECK (splitImpulses (std::vector<float> (100, 0.0f), std::vector<float> (100, 0.0f), kFs).empty());
}
