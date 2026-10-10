#include "BrainListener.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis::brain
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

float toDb (double power) noexcept
{
    return static_cast<float> (10.0 * std::log10 (power + 1.0e-12));
}

float activity (float db, float floorDb) noexcept
{
    return std::clamp ((db - floorDb) / BrainListener::kRangeDb, 0.0f, 1.3f);
}

/** Power sum with an activity-weighted colour, for one class and group of bands. */
struct Accumulator
{
    double power = 0.0;
    float wr = 0.0f, wg = 0.0f, wb = 0.0f, w = 0.0f;
    void add (float p, float x, const std::array<float, 3>& rgb) noexcept
    {
        power += p;
        wr += x * rgb[0];
        wg += x * rgb[1];
        wb += x * rgb[2];
        w += x;
    }
    void write (HopFrame::Part& part) const noexcept
    {
        part.x = power > 0.0 ? activity (toDb (power), BrainListener::kSumFloorDb) : 0.0f;
        if (w > 0.0f)
        {
            part.r = wr / w;
            part.g = wg / w;
            part.b = wb / w;
        }
    }
};
} // namespace

void BrainListener::Feature::add (float p) noexcept
{
    const float reference = (power[0] + power[1] + power[2]) / 3.0f;
    power[3] = power[2];
    power[2] = power[1];
    power[1] = power[0];
    power[0] = p;
    db = toDb (p);
    rise = db - toDb (reference);
}

BrainListener::BrainListener()
{
    ringL.assign (static_cast<size_t> (kRing), 0.0f);
    ringR.assign (static_cast<size_t> (kRing), 0.0f);
    for (int b = 0; b < kBands; ++b)
        bandRgb[static_cast<size_t> (b)] = frequencyRgb (bandHz (b));
    setSampleRate (48000.0);
}

void BrainListener::setSampleRate (double rate)
{
    if (rate <= 0.0 || std::abs (rate - sampleRate) < 0.5)
        return;
    sampleRate = rate;
    int m = 1, order = 0;
    while (m < 4 && rate / (48000.0 * m) > 1.5)
    {
        m *= 2;
        ++order;
    }
    shortSize = 1024 * m;
    longSize = 4096 * m;
    hop = 512 * m;
    shortFft = std::make_unique<juce::dsp::FFT> (10 + order);
    longFft = std::make_unique<juce::dsp::FFT> (12 + order);
    const auto hann = [] (std::vector<float>& w, int n)
    {
        w.resize (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            w[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * i / n)); // periodic: sum N / 2, sum of squares 3N / 8
    };
    hann (shortWindow, shortSize);
    hann (longWindow, longSize);
    shortIn.assign (static_cast<size_t> (shortSize), {});
    shortOut.assign (static_cast<size_t> (shortSize), {});
    longIn.assign (static_cast<size_t> (longSize), {});
    longOut.assign (static_cast<size_t> (longSize), {});
    shortL.assign (static_cast<size_t> (shortSize / 2 + 1), 0.0f);
    shortR.assign (static_cast<size_t> (shortSize / 2 + 1), 0.0f);
    longL.assign (static_cast<size_t> (longSize / 2 + 1), 0.0f);
    longR.assign (static_cast<size_t> (longSize / 2 + 1), 0.0f);

    const double ratio = std::pow (kHighHz / kLowHz, 1.0 / (kBands - 1));
    for (int b = 0; b < kBands; ++b)
    {
        auto& m2 = bands[static_cast<size_t> (b)];
        const double f = bandHz (b);
        const double lo = b == 0 ? f / std::sqrt (ratio) : std::sqrt (bandHz (b - 1) * f);
        const double hi = b == kBands - 1 ? f * std::sqrt (ratio) : std::sqrt (f * bandHz (b + 1));
        m2.longWindow = f < kSplitHz;
        const int n = m2.longWindow ? longSize : shortSize;
        const double binHz = rate / n;
        m2.k0 = std::max (1, static_cast<int> (std::ceil (lo / binHz)));
        m2.k1 = std::min (n / 2 - 1, static_cast<int> (std::floor (hi / binHz)));
        m2.centreBin = std::min (f / binHz, static_cast<double> (n / 2 - 2));
        m2.interpolate = m2.k1 - m2.k0 + 1 < 3;
    }
    reset();
}

void BrainListener::reset() noexcept
{
    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    received = processed = hops = 0;
    offset = 0.0;
    lastHopTime = -1.0e9;
    offsetValid = false;
    for (auto& e : bandDb)
        e.fill (-120.0f);
    for (auto& e : levelPower)
        e.fill (0.0f);
    for (auto& e : bandHistory)
        for (auto& h : e)
            h.fill (0.0f);
    for (auto& e : sinceOnset)
        e.fill (1000);
    for (auto& e : onsetCounts)
        e.fill (0);
    low = click = body = Feature {};
    candidate = Candidate {};
    lastBeatTime = -1.0e9;
    kicks = snares = 0;
    beatLog.fill (0.0);
    beatLogCount = beatLogNext = 0;
    slots.fill (Slot {});
    slotsFilled = 0;
    slotNewest = -1;
    currentSlot = -1;
    slotTotal = slotLow = slotHigh = 0.0;
    slotHops = slotHits = 0;
    momLow = momTotal = 0.0f;
    buildActive = false;
    buildRising = buildQuiet = buildUps = dropCount = 0;
    buildStart = nextAnticipation = 0.0;
    buildEnd = lastDropTime = -1.0e9;
    musicListener.reset();
    musicListener.setSampleRate (sampleRate);
    tracker.reset();
    lastSurprise = -1;
}

double BrainListener::getBeatTime (int i) const noexcept
{
    const int oldestIndex = beatLogCount < kBeatLog ? 0 : beatLogNext;
    return beatLog[static_cast<size_t> ((oldestIndex + i) % kBeatLog)];
}

void BrainListener::push (const float* mid, const float* side, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const auto at = static_cast<size_t> ((received + i) & (kRing - 1));
        ringL[at] = mid[i] + side[i];
        ringR[at] = mid[i] - side[i];
    }
    received += numSamples;
    musicListener.push (mid, numSamples);
}

void BrainListener::advance (double clock, double dt, BrainActivity& sink) noexcept
{
    // Display time of the stream: the newest sample is "now", smoothed against the feed's jitter.
    const double target = clock - static_cast<double> (received) / sampleRate;
    if (! offsetValid || std::abs (target - offset) > 0.15)
    {
        offset = target;
        offsetValid = true;
    }
    else
    {
        offset += (target - offset) * 0.05;
    }

    const int64_t pendingHops = (received - processed) / hop;
    if (pendingHops > kMaxHopsPerFrame)
        processed += (pendingHops - kMaxHopsPerFrame) * hop;
    while (processed + hop <= received)
    {
        processed += hop;
        ++hops;
        double t = offset + static_cast<double> (processed - longSize / 2) / sampleRate;
        t = std::max (t, lastHopTime + 1.0e-4);
        lastHopTime = t;
        analyseHop (processed, t, sink);
    }
    updateChords (clock, dt, sink);
}

void BrainListener::transform (int size, int64_t centre, juce::dsp::FFT& fft, const std::vector<float>& window,
                               std::vector<juce::dsp::Complex<float>>& in, std::vector<juce::dsp::Complex<float>>& out, std::vector<float>& powerL,
                               std::vector<float>& powerR) noexcept
{
    const int64_t start = centre - size / 2;
    for (int n = 0; n < size; ++n)
    {
        const auto at = static_cast<size_t> ((start + n) & (kRing - 1));
        const float w = window[static_cast<size_t> (n)];
        in[static_cast<size_t> (n)] = { ringL[at] * w, ringR[at] * w };
    }
    fft.perform (in.data(), out.data(), false);
    // Z = FFT (L + iR): L_k = (Z_k + conj Z_{N-k}) / 2, R_k = (Z_k - conj Z_{N-k}) / 2i.
    for (int k = 0; k <= size / 2; ++k)
    {
        const auto& z = out[static_cast<size_t> (k)];
        const auto& m = out[static_cast<size_t> ((size - k) % size)];
        const float a = z.real(), b = z.imag(), c = m.real(), d = m.imag();
        powerL[static_cast<size_t> (k)] = 0.25f * ((a + c) * (a + c) + (b - d) * (b - d));
        powerR[static_cast<size_t> (k)] = 0.25f * ((b + d) * (b + d) + (a - c) * (a - c));
    }
}

float BrainListener::bandPower (const BandMap& m, const std::vector<float>& shortP, const std::vector<float>& longP) const noexcept
{
    const auto& p = m.longWindow ? longP : shortP;
    const double n = m.longWindow ? longSize : shortSize;
    if (m.interpolate)
    {
        const auto k = static_cast<int> (std::floor (m.centreBin));
        const double frac = m.centreBin - k;
        const double v = p[static_cast<size_t> (k)] * (1.0 - frac) + p[static_cast<size_t> (k + 1)] * frac;
        return static_cast<float> (v * 16.0 / (n * n)); // a bin-centred sine of amplitude a: a^2
    }
    double sum = 0.0;
    for (int k = m.k0; k <= m.k1; ++k)
        sum += p[static_cast<size_t> (k)];
    return static_cast<float> (sum * 32.0 / (3.0 * n * n)); // a sine inside the band: a^2
}

float BrainListener::rangeSum (const std::vector<float>& p, double fromHz, double toHz, int size) const noexcept
{
    const double binHz = sampleRate / size;
    const int k0 = std::max (1, static_cast<int> (std::ceil (fromHz / binHz)));
    const int k1 = std::min (size / 2 - 1, static_cast<int> (std::floor (toHz / binHz)));
    double sum = 0.0;
    for (int k = k0; k <= k1; ++k)
        sum += p[static_cast<size_t> (k)];
    return static_cast<float> (sum * 32.0 / (3.0 * static_cast<double> (size) * size));
}

void BrainListener::analyseHop (int64_t hopEnd, double time, BrainActivity& sink) noexcept
{
    // Both windows centred on the same instant (the long window's centre).
    const int64_t centre = hopEnd - longSize / 2;
    transform (longSize, centre, *longFft, longWindow, longIn, longOut, longL, longR);
    transform (shortSize, centre, *shortFft, shortWindow, shortIn, shortOut, shortL, shortR);

    const double hopSeconds = static_cast<double> (hop) / sampleRate;
    const auto release = static_cast<float> (1.0 - std::exp (-hopSeconds / 0.08));
    const int refractoryHops = static_cast<int> (std::ceil (0.08 / hopSeconds));

    HopFrame frame;
    frame.time = time;
    std::array<std::array<bool, kBands>, 2> onset {};
    float totalP = 0.0f, lowP = 0.0f, highP = 0.0f;
    int hfOnsets = 0;
    for (size_t e = 0; e < 2; ++e)
    {
        const auto& sp = e == 0 ? shortL : shortR;
        const auto& lp = e == 0 ? longL : longR;
        int count = 0;
        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
        {
            const auto& map = bands[b];
            const float p = bandPower (map, sp, lp);
            const float db = toDb (p);
            bandDb[e][b] = db;
            auto& history = bandHistory[e][b];
            const int depth = map.longWindow ? 6 : 3;
            float mean = 0.0f;
            for (int i = 0; i < depth; ++i)
                mean += history[static_cast<size_t> (i)];
            mean /= static_cast<float> (depth);
            const float rise = db - toDb (mean);
            for (size_t i = history.size() - 1; i > 0; --i)
                history[i] = history[i - 1];
            history[0] = p;
            auto& level = levelPower[e][b];
            level = p > level ? p : level + (p - level) * release;
            auto& since = sinceOnset[e][b];
            since = std::min (since + 1, 1000);
            if (rise >= kOnsetDb && db >= kOnsetFloorDb && since >= refractoryHops)
            {
                onset[e][b] = true;
                since = 0;
                ++count;
                if (bandHz (static_cast<int> (b)) >= 2000.0)
                    ++hfOnsets;
            }
            totalP += p;
            const double hz = bandHz (static_cast<int> (b));
            if (hz >= 40.0 && hz <= 150.0)
                lowP += p;
            if (hz >= 4000.0)
                highP += p;
        }
        auto& counts = onsetCounts[e];
        counts[2] = counts[1];
        counts[1] = counts[0];
        counts[0] = count;
        const bool broad = counts[0] + counts[1] + counts[2] >= kBroadBands;

        std::array<std::array<Accumulator, HopFrame::kGroups>, HopFrame::kClasses> acc {};
        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
        {
            const size_t group = bandHz (static_cast<int> (b)) < kLowHighSplitHz ? HopFrame::Low : HopFrame::High;
            const auto& rgb = bandRgb[b];
            const float levelPowerNow = levelPower[e][b];
            const float xl = activity (toDb (levelPowerNow), kBandFloorDb);
            frame.band[e][HopFrame::Level][b] = xl;
            acc[HopFrame::Level][HopFrame::All].add (levelPowerNow, xl, rgb);
            acc[HopFrame::Level][group].add (levelPowerNow, xl, rgb);
            if (onset[e][b])
            {
                const size_t cls = broad ? HopFrame::OnsetPercussive : HopFrame::OnsetTonal;
                const float xo = activity (bandDb[e][b], kBandFloorDb);
                const float p = static_cast<float> (std::pow (10.0, bandDb[e][b] / 10.0));
                frame.band[e][cls][b] = xo;
                acc[cls][HopFrame::All].add (p, xo, rgb);
                acc[cls][group].add (p, xo, rgb);
            }
        }
        for (size_t c = 0; c < static_cast<size_t> (HopFrame::kClasses); ++c)
            for (size_t g = 0; g < static_cast<size_t> (HopFrame::kGroups); ++g)
                acc[c][g].write (frame.ear[e][c][g]);
    }
    sink.addHop (frame);

    // Beat features from the short window, L + R: the bands' power, the low band's spectral centroid (a kick drum's
    // pitch falls in its first tens of milliseconds) and the 1 - 4 kHz flatness (a snare is noise, a note is not).
    {
        const double binHz = sampleRate / shortSize;
        double centroidSum = 0.0, centroidWeight = 0.0, logSum = 0.0, linSum = 0.0, all = 0.0;
        int flatBins = 0;
        for (int k = 1; k < shortSize / 2; ++k)
        {
            const double p = static_cast<double> (shortL[static_cast<size_t> (k)]) + shortR[static_cast<size_t> (k)];
            const double hz = k * binHz;
            all += p;
            if (hz >= 40.0 && hz <= 250.0)
            {
                centroidSum += hz * p;
                centroidWeight += p;
            }
            if (hz >= 1000.0 && hz <= 4000.0)
            {
                logSum += std::log (p + 1.0e-20);
                linSum += p;
                ++flatBins;
            }
        }
        const double scale = 32.0 / (3.0 * static_cast<double> (shortSize) * shortSize);
        low.add (rangeSum (shortL, 40.0, 150.0, shortSize) + rangeSum (shortR, 40.0, 150.0, shortSize));
        click.add (rangeSum (shortL, 1000.0, 4000.0, shortSize) + rangeSum (shortR, 1000.0, 4000.0, shortSize));
        body.add (rangeSum (shortL, 150.0, 500.0, shortSize) + rangeSum (shortR, 150.0, 500.0, shortSize));
        const float centroid = centroidWeight > 0.0 ? static_cast<float> (centroidSum / centroidWeight) : 0.0f;
        const float flatness = flatBins > 0 && linSum > 0.0 ? static_cast<float> (std::exp (logSum / flatBins) / (linSum / flatBins)) : 0.0f;
        detectBeat (time, centroid, flatness, toDb (all * scale), sink);
    }

    updateMacro (time, static_cast<double> (hopEnd) / sampleRate, totalP, lowP, highP, hfOnsets >= 3, sink);
}

void BrainListener::detectBeat (double time, float centroid, float flatness, float allDb, BrainActivity& sink) noexcept
{
    // A low-band onset (the kick's thump) and a noisy 1 - 4 kHz onset with some body (a snare; hi-hats have none).
    const bool lowOnset = low.rise >= kKickRiseDb && low.db >= -45.0f && low.db >= allDb - 12.0f;
    const bool snareOnset = click.rise >= kSnareRiseDb && click.db >= allDb - 20.0f && flatness >= kSnareFlatness && body.rise >= kBodyRiseDb
                            && body.db >= allDb - 25.0f;
    if (! candidate.active)
    {
        if (! lowOnset && ! snareOnset)
            return;
        candidate = Candidate {};
        candidate.active = true;
        candidate.time = time;
        candidate.lowLike = lowOnset;
        candidate.snareLike = snareOnset;
        candidate.centroids[0] = centroid;
        candidate.lowDb = low.db;
        candidate.clickDb = click.db;
        return;
    }
    // Decided kLookahead hops later. A kick drum's pitch falls: its low-band centroid sinks smoothly (no step up
    // of more than kSweepStepHz) to at most kSweep of where it started; a bass note's stays (or jitters).
    ++candidate.age;
    candidate.centroids[static_cast<size_t> (candidate.age)] = centroid;
    if (candidate.age == 1)
    {
        candidate.lowLike = candidate.lowLike || lowOnset;
        candidate.snareLike = candidate.snareLike || snareOnset;
        candidate.lowDb = std::max (candidate.lowDb, low.db);
        candidate.clickDb = std::max (candidate.clickDb, click.db);
    }
    if (candidate.age < kLookahead)
        return;
    candidate.active = false;
    const auto& c = candidate.centroids;
    bool smooth = c[0] > 0.0f;
    for (size_t k = 1; k < c.size(); ++k)
        smooth = smooth && c[k] > 0.0f && c[k] <= c[k - 1] + kSweepStepHz;
    const bool kick = candidate.lowLike && smooth && c.back() <= kSweep * std::max (c[0], c[1]);
    // A snare is still noise when decided (an onset from silence is flat only for a moment).
    const bool snare = ! kick && candidate.snareLike && flatness >= kSnareFlatness * 0.8f;
    if (! (kick || snare) || candidate.time - lastBeatTime < kBeatRefractory)
        return;
    lastBeatTime = candidate.time;
    const auto strength = [] (float db) { return std::clamp ((db + 60.0f) / 50.0f, 0.0f, 1.0f); };
    if (kick)
        ++kicks;
    else
        ++snares;
    sink.beat (candidate.time, kick ? strength (candidate.lowDb) : 0.7f * strength (candidate.clickDb + 10.0f));
    beatLog[static_cast<size_t> (beatLogNext)] = candidate.time;
    beatLogNext = (beatLogNext + 1) % kBeatLog;
    beatLogCount = std::min (beatLogCount + 1, kBeatLog);
}

void BrainListener::updateMacro (double time, double streamSeconds, float totalP, float lowP, float highP, bool hfHit, BrainActivity& sink) noexcept
{
    const double hopSeconds = static_cast<double> (hop) / sampleRate;
    const auto ageMean = [this] (int from, int to, float Slot::*field)
    {
        float sum = 0.0f;
        int n = 0;
        for (int age = from; age <= to && age < slotsFilled; ++age)
        {
            sum += std::max (-90.0f, slots[static_cast<size_t> (((slotNewest - age) % kSlots + kSlots) % kSlots)].*field);
            ++n;
        }
        return n > 0 ? sum / static_cast<float> (n) : -90.0f;
    };
    const auto ageHits = [this] (int from, int to)
    {
        int sum = 0;
        for (int age = from; age <= to && age < slotsFilled; ++age)
            sum += slots[static_cast<size_t> (((slotNewest - age) % kSlots + kSlots) % kSlots)].hfHits;
        return sum;
    };

    // ---- 0.25 s slots and the build-up -----------------------------------------------------------
    const auto slotIndex = static_cast<int64_t> (std::floor (streamSeconds / kSlotSeconds));
    if (currentSlot < 0)
        currentSlot = slotIndex;
    if (slotIndex != currentSlot && slotHops > 0)
    {
        slotNewest = (slotNewest + 1) % kSlots;
        auto& s = slots[static_cast<size_t> (slotNewest)];
        s.total = toDb (slotTotal / slotHops);
        s.low = toDb (slotLow / slotHops);
        s.high = toDb (slotHigh / slotHops);
        s.hfHits = slotHits;
        slotsFilled = std::min (slotsFilled + 1, kSlots);
        slotTotal = slotLow = slotHigh = 0.0;
        slotHops = slotHits = 0;
        currentSlot = slotIndex;

        if (slotsFilled >= 12)
        {
            const float now = ageMean (0, 3, &Slot::total), mid = ageMean (4, 7, &Slot::total), then = ageMean (8, 11, &Slot::total);
            const float hNow = ageMean (0, 3, &Slot::high), hMid = ageMean (4, 7, &Slot::high), hThen = ageMean (8, 11, &Slot::high);
            const int hfNow = ageHits (0, 3), hfThen = ageHits (8, 11);
            // Not a build-up: the bass coming back (that is a drop) or the seconds after a drop.
            const float lowRecent = ageMean (0, 3, &Slot::low);
            const bool bassJump = lowRecent >= -45.0f && lowRecent >= now - 12.0f && lowRecent >= ageMean (8, 11, &Slot::low) + 6.0f;
            const bool rising = now >= -60.0f && ! bassJump && time - lastDropTime >= 4.0
                                && ((now - then >= 2.5f && mid >= then + 0.5f && now >= mid + 0.5f)
                                    || (hNow - hThen >= 4.0f && hMid >= hThen + 1.0f && hNow >= hMid + 1.0f) || (hfNow >= hfThen + 3 && hfNow >= 6));
            if (rising)
            {
                ++buildRising;
                buildQuiet = 0;
            }
            else
            {
                buildRising = 0;
                ++buildQuiet;
            }
            if (! buildActive && buildRising >= 2 && streamSeconds >= 4.0)
            {
                buildActive = true;
                buildStart = time;
                nextAnticipation = time;
                ++buildUps;
            }
            if (buildActive && buildQuiet >= 4)
            {
                buildActive = false;
                buildEnd = time;
            }
        }
    }
    slotTotal += totalP;
    slotLow += lowP;
    slotHigh += highP;
    ++slotHops;
    slotHits += hfHit ? 1 : 0;

    if (buildActive && time >= nextAnticipation)
    {
        sink.anticipation (time, static_cast<float> (std::clamp ((time - buildStart + 1.0) / 5.0, 0.0, 1.0)));
        nextAnticipation = time + 0.5;
    }

    // ---- The drop ------------------------------------------------------------------------------
    momLow += (lowP - momLow) * static_cast<float> (1.0 - std::exp (-hopSeconds / 0.1));
    momTotal += (totalP - momTotal) * static_cast<float> (1.0 - std::exp (-hopSeconds / 0.15));
    if (slotsFilled < 16 || streamSeconds < 4.0 || time - lastDropTime < 6.0)
        return;
    const float refLow = ageMean (0, 7, &Slot::low), refTotal = ageMean (0, 7, &Slot::total);
    const float lowNow = toDb (momLow), totalNow = toDb (momTotal);
    const bool afterBuild = buildActive || time - buildEnd <= 1.5;
    bool quieter = false;
    if (slotsFilled >= 24)
        quieter = refTotal <= ageMean (8, 39, &Slot::total) - 5.0f && refTotal >= -55.0f;
    // The low end returns: 10 dB over the 2 s before, and it carries the mix again (within 9 dB of the whole).
    const bool jump = lowNow >= refLow + 10.0f && lowNow >= -40.0f && lowNow >= totalNow - 9.0f && totalNow >= refTotal + (afterBuild ? 1.0f : 3.0f);
    if (jump && (afterBuild || quieter))
    {
        sink.drop (time);
        ++dropCount;
        lastDropTime = time;
        buildActive = false;
        buildEnd = time;
        buildRising = 0;
    }
}

void BrainListener::updateChords (double clock, double dt, BrainActivity& sink) noexcept
{
    musicListener.advance (dt);
    const auto& est = musicListener.estimator;
    const music::Chord before = tracker.getChord();
    if (! tracker.update (&est.getNote (0), est.getNumNotes(), dt))
        return;
    const auto& chord = tracker.getChord();
    if (! chord.isChord() || ! before.isChord())
        return;
    const int key = musicListener.key.getKey();
    bool surprise = false;
    if (key >= 0 && musicListener.key.getConfidence() >= 0.3f)
    {
        auto scale = static_cast<unsigned> (music::scaleMask (key));
        if (key >= 12) // minor: also the melodic minor's raised sixth and seventh (IV and V major are expected)
            scale |= (1u << (((key - 12) + 9) % 12)) | (1u << (((key - 12) + 11) % 12));
        surprise = (chord.mask & ~scale & 0xfffu) != 0;
    }
    lastSurprise = surprise ? 1 : 0;
    sink.chord (clock, surprise ? 1.0f : 0.22f, surprise ? 0.4f : 0.1f);
}
} // namespace flub::app::ui::vis::brain
