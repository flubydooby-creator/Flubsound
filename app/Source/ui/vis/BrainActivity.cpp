#include "BrainActivity.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis::brain
{
namespace
{
constexpr int kPoints = BrainAnatomy::kSegments + 1;

/** A mix of parts: x-weighted colour. */
struct Mix
{
    float x = 0.0f, wr = 0.0f, wg = 0.0f, wb = 0.0f, w = 0.0f;
    void add (float value, float r, float g, float b) noexcept
    {
        if (value <= 0.0f)
            return;
        x = std::max (x, value);
        wr += value * r;
        wg += value * g;
        wb += value * b;
        w += value;
    }
    void colour (float& r, float& g, float& b) const noexcept
    {
        if (w <= 0.0f)
        {
            r = g = b = 0.0f;
            return;
        }
        r = wr / w;
        g = wg / w;
        b = wb / w;
    }
};

using Part = HopFrame::Part;

/** a x pa + b x pb: the activity of two ears seen from one side (x and colour). */
Part blend (const Part& pa, float a, const Part& pb, float b) noexcept
{
    Part p;
    const float xa = pa.x * a, xb = pb.x * b;
    p.x = xa + xb;
    const float w = xa + xb;
    if (w > 0.0f)
    {
        p.r = (xa * pa.r + xb * pb.r) / w;
        p.g = (xa * pa.g + xb * pb.g) / w;
        p.b = (xa * pa.b + xb * pb.b) / w;
    }
    return p;
}

/** The LSO's view of a part: its own ear's activity less kLsoInhibition x the other ear's (never below 0), its colour. */
Part inhibited (const Part& own, const Part& other) noexcept
{
    Part p = own;
    p.x = std::max (0.0f, own.x - kLsoInhibition * other.x);
    return p;
}

float decayFactor (double dt, float seconds) noexcept
{
    return static_cast<float> (std::exp (-std::max (0.0, dt) / seconds));
}

/** Held activity that has decayed out of sight is 0 (no denormals; a faded brain is exactly dark). */
constexpr float kGone = 1.0e-4f;
inline void decay (float& x, float factor) noexcept
{
    x *= factor;
    if (x < kGone)
        x = 0.0f;
}

/** Normalised colour (max channel 1). */
void normalise (float& r, float& g, float& b) noexcept
{
    const float m = std::max ({ r, g, b });
    if (m > 1.0e-6f)
    {
        r /= m;
        g /= m;
        b /= m;
    }
}
} // namespace

BrainActivity::BrainActivity (const BrainAnatomy& a) : anatomy (a)
{
    history.resize (static_cast<size_t> (kHistory));
    const size_t n = anatomy.getTracts().size() * static_cast<size_t> (kPoints);
    segmentHeld.resize (n);
    segments.resize (n);
    eventAccum.resize (n);
    for (int side = kLeft; side <= kRight; ++side)
    {
        const char* suffix = side == kLeft ? "-L" : "-R";
        auto& e = event[static_cast<size_t> (side)];
        const auto find = [this, suffix] (const char* base)
        {
            char name[48] = {};
            size_t i = 0;
            for (; base[i] != '\0' && i < 40; ++i)
                name[i] = base[i];
            name[i] = suffix[0];
            name[i + 1] = suffix[1];
            return anatomy.findTract (name);
        };
        e.pons = find ("pons-cerebellum");
        e.cerebellumThalamus = find ("cerebellum-thalamus");
        e.thalamusPremotor = find ("thalamus-premotor");
        e.dorsal = find ("dorsal");
        e.premotorSma = find ("premotor-sma");
        e.smaPutamen = find ("sma-putamen");
        e.ventral = find ("ventral");
        e.heschlAccumbens = find ("heschl-accumbens");
        e.vtaAccumbens = find ("vta-accumbens");
        e.vtaCaudate = find ("vta-caudate");
    }
    reset();
}

void BrainActivity::reset() noexcept
{
    oldest = count = 0;
    lastPulseSlot = -1.0;
    std::fill (segmentHeld.begin(), segmentHeld.end(), Held {});
    std::fill (segments.begin(), segments.end(), Glow {});
    std::fill (eventAccum.begin(), eventAccum.end(), std::array<float, 4> {});
    nodeHeld.fill (Held {});
    nodeGlow.fill (Glow {});
    for (auto& side : hgHeld)
        side.fill (0.0f);
    for (auto& side : cochHeld)
        side.fill (0.0f);
    earHeld.fill (0.0f);
    for (auto& w : waves)
        w.active = false;
    nextWave = 0;
    now = previous = 0.0;
    started = false;
    dark = true;
    beats = chords = surprises = anticipations = drops = 0;
    lastBeat = lastDrop = lastAnticipation = lastSurprise = -1.0e9;
}

float BrainActivity::brightness (float x) noexcept
{
    if (x <= 0.0f)
        return 0.0f;
    return std::pow (std::min (x, 1.3f), 1.2f);
}

float BrainActivity::heschlPeak (int side) const noexcept
{
    float best = 0.0f;
    for (const float v : hgHeld[static_cast<size_t> (side)])
        best = std::max (best, v);
    return brightness (best);
}

int BrainActivity::getActiveWaves() const noexcept
{
    int n = 0;
    for (const auto& w : waves)
        n += w.active ? 1 : 0;
    return n;
}

// =============================================================================
// Input
// =============================================================================
void BrainActivity::addHop (const HopFrame& f) noexcept
{
    if (count > 0 && f.time < record (count - 1).time)
        return; // out of order: ignore
    Record* r = nullptr;
    if (count < kHistory)
    {
        r = &history[static_cast<size_t> ((oldest + count) % kHistory)];
        ++count;
    }
    else
    {
        r = &history[static_cast<size_t> (oldest)];
        oldest = (oldest + 1) % kHistory;
    }
    r->time = f.time;
    const double slot = std::floor (f.time / kPulseSeconds);
    const bool pulse = slot != lastPulseSlot;
    lastPulseSlot = slot;

    const auto drive = [pulse] (const Part& onT, const Part& onP, const Part& lev, float wT, float wP, Drive& d)
    {
        Mix tract, region;
        tract.add (onT.x * wT, onT.r, onT.g, onT.b);
        tract.add (onP.x * wP, onP.r, onP.g, onP.b);
        tract.add ((pulse ? kPulse : kFloor) * lev.x * wT, lev.r, lev.g, lev.b);
        region.add (onT.x * wT, onT.r, onT.g, onT.b);
        region.add (onP.x * wP, onP.r, onP.g, onP.b);
        region.add (kSteady * lev.x * wT, lev.r, lev.g, lev.b);
        d.tract = tract.x;
        d.region = region.x;
        tract.colour (d.r, d.g, d.b);
        if (tract.w <= 0.0f)
            region.colour (d.r, d.g, d.b);
    };
    using C = HopFrame::Class;
    using G = HopFrame::Group;
    for (int side = kLeft; side <= kRight; ++side)
    {
        const int other = 1 - side;
        const auto& me = f.ear[static_cast<size_t> (side)];
        const auto& op = f.ear[static_cast<size_t> (other)];
        const auto channel = [r, side] (Channel c) -> Drive& { return r->channel[static_cast<size_t> (channelIndex (c, side))]; };
        const struct
        {
            Channel ear;
            G group;
        } groups[] = { { Channel::EarAll, G::All }, { Channel::EarLow, G::Low }, { Channel::EarHigh, G::High } };
        for (const auto& gr : groups)
        {
            const auto g = static_cast<size_t> (gr.group);
            drive (me[C::OnsetTonal][g], me[C::OnsetPercussive][g], me[C::Level][g], 1.0f, 1.0f, channel (gr.ear));
        }
        // The superior olive: the MSO hears both ears alike (low bands), the LSO its own ear less the other one (high bands).
        const auto low = static_cast<size_t> (G::Low), high = static_cast<size_t> (G::High), all = static_cast<size_t> (G::All);
        drive (blend (me[C::OnsetTonal][low], kMsoEachEar, op[C::OnsetTonal][low], kMsoEachEar),
               blend (me[C::OnsetPercussive][low], kMsoEachEar, op[C::OnsetPercussive][low], kMsoEachEar),
               blend (me[C::Level][low], kMsoEachEar, op[C::Level][low], kMsoEachEar), 1.0f, 1.0f, channel (Channel::OliveLow));
        drive (inhibited (me[C::OnsetTonal][high], op[C::OnsetTonal][high]), inhibited (me[C::OnsetPercussive][high], op[C::OnsetPercussive][high]),
               inhibited (me[C::Level][high], op[C::Level][high]), 1.0f, 1.0f, channel (Channel::OliveHigh));
        // From the lateral lemniscus up: 35 % this side's ear, 65 % the other ear.
        const Part ascendT = blend (me[C::OnsetTonal][all], kIpsilateral, op[C::OnsetTonal][all], kContralateral);
        const Part ascendP = blend (me[C::OnsetPercussive][all], kIpsilateral, op[C::OnsetPercussive][all], kContralateral);
        const Part ascendL = blend (me[C::Level][all], kIpsilateral, op[C::Level][all], kContralateral);
        drive (ascendT, ascendP, ascendL, 1.0f, 1.0f, channel (Channel::AscendAll));
        // The radiation to the cortex: percussive onsets weigh more on the left, tonal sound on the right.
        const float wT = side == kRight ? kTonalRight : kOtherSide;
        const float wP = side == kLeft ? kPercussiveLeft : kOtherSide;
        drive (ascendT, ascendP, ascendL, wT, wP, channel (Channel::Radiation));
        // Per band: the cochlea (one ear) and Heschl's gyrus (this side above the olive).
        const auto& bm = f.band[static_cast<size_t> (side)];
        const auto& bo = f.band[static_cast<size_t> (other)];
        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
        {
            r->coch[static_cast<size_t> (side)][b] = std::max ({ bm[C::OnsetTonal][b], bm[C::OnsetPercussive][b], kSteady * bm[C::Level][b] });
            const float onT = kIpsilateral * bm[C::OnsetTonal][b] + kContralateral * bo[C::OnsetTonal][b];
            const float onP = kIpsilateral * bm[C::OnsetPercussive][b] + kContralateral * bo[C::OnsetPercussive][b];
            const float lev = kIpsilateral * bm[C::Level][b] + kContralateral * bo[C::Level][b];
            r->hg[static_cast<size_t> (side)][b] = std::max ({ onT * wT, onP * wP, kSteady * lev * wT });
        }
    }
}

void BrainActivity::addWave (int tract, double start, float duration, float amplitude, const std::array<float, 3>& rgb) noexcept
{
    if (tract < 0 || amplitude < 0.02f)
        return;
    // A free slot, else the oldest (the ring order).
    int slot = -1;
    for (int k = 0; k < kMaxWaves; ++k)
    {
        const int i = (nextWave + k) % kMaxWaves;
        if (! waves[static_cast<size_t> (i)].active)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        slot = nextWave;
    nextWave = (slot + 1) % kMaxWaves;
    auto& w = waves[static_cast<size_t> (slot)];
    w.active = true;
    w.hit = false;
    w.tract = tract;
    w.start = start;
    w.duration = duration;
    w.amplitude = amplitude;
    w.rgb = rgb;
}

void BrainActivity::hitNode (int nodeIndex, float amplitude, const std::array<float, 3>& rgb) noexcept
{
    nodeHeld[static_cast<size_t> (nodeIndex)].take (amplitude, rgb[0], rgb[1], rgb[2]);
}

void BrainActivity::beat (double t, float strength) noexcept
{
    const float a = std::clamp (strength, 0.0f, 1.0f);
    ++beats;
    lastBeat = t;
    for (const auto& e : event)
    {
        addWave (e.pons, t + 0.13, 0.05f, 0.55f * a, kBeatRgb);
        addWave (e.cerebellumThalamus, t + 0.18, 0.16f, 0.45f * a, kBeatRgb);
        addWave (e.thalamusPremotor, t + 0.34, 0.12f, 0.45f * a, kBeatRgb);
        addWave (e.dorsal, t + 0.32, 0.16f, 0.6f * a, kBeatRgb);
        addWave (e.premotorSma, t + 0.48, 0.1f, 0.6f * a, kBeatRgb);
        addWave (e.smaPutamen, t + 0.58, 0.1f, 0.5f * a, kBeatRgb);
    }
}

void BrainActivity::chord (double t, float right, float left) noexcept
{
    ++chords;
    if (right >= 0.5f)
    {
        ++surprises;
        lastSurprise = t;
    }
    addWave (event[kRight].ventral, t + 0.1, 0.12f, std::clamp (right, 0.0f, 1.0f), kChordRgb);
    addWave (event[kLeft].ventral, t + 0.1, 0.12f, std::clamp (left, 0.0f, 1.0f), kChordRgb);
}

void BrainActivity::anticipation (double t, float progress) noexcept
{
    ++anticipations;
    lastAnticipation = t;
    const float p = std::clamp (progress, 0.0f, 1.0f);
    for (const auto& e : event)
        addWave (e.vtaCaudate, t, 0.25f, 0.3f + 0.6f * p, kDopamineRgb);
    hitNode (node (Station::Vta, kLeft), 0.45f + 0.4f * p, kDopamineRgb);
}

void BrainActivity::drop (double t) noexcept
{
    ++drops;
    lastDrop = t;
    for (const auto& e : event)
    {
        addWave (e.vtaAccumbens, t + 0.05, 0.22f, 1.0f, kDopamineRgb);
        addWave (e.heschlAccumbens, t + 0.35, 0.22f, 0.8f, kDopamineRgb);
    }
    hitNode (node (Station::Vta, kLeft), 1.0f, kDopamineRgb);
}

// =============================================================================
// Per frame
// =============================================================================
int BrainActivity::firstAfter (double t) const noexcept
{
    int lo = 0, hi = count;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (record (mid).time > t)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

void BrainActivity::update (double t, double dt) noexcept
{
    if (! started)
    {
        previous = t - std::max (dt, 1.0e-3);
        started = true;
    }
    else
    {
        previous = now;
    }
    now = t;
    const double frame = std::max (0.0, now - previous);

    const auto& tracts = anatomy.getTracts();
    const float trail = decayFactor (frame, kTrailSeconds), regionDecay = decayFactor (frame, kRegionSeconds);
    const float hgDecay = decayFactor (frame, kHeschlSeconds), cochDecay = decayFactor (frame, kCochleaSeconds);
    float brightest = 0.0f; // of every tract segment and node (Heschl's gyrus and the cochlea: their strongest band)

    // ---- Ascending pathway: delayed readout of the history ----------------------------------
    for (size_t ti = 0; ti < tracts.size(); ++ti)
    {
        const auto& tr = tracts[ti];
        Held* held = &segmentHeld[ti * kPoints];
        Glow* glow = &segments[ti * kPoints];
        if (tr.kind != TractKind::Auditory)
            continue;
        const size_t ch = static_cast<size_t> (channelIndex (tr.channel, tr.channelSide));
        for (int j = 0; j < kPoints; ++j)
        {
            auto& h = held[j];
            decay (h.x, trail);
            const double d = tr.delayStart + (tr.delayEnd - tr.delayStart) * static_cast<double> (j) / BrainAnatomy::kSegments;
            const double a = previous - d, b = now - d;
            for (int k = firstAfter (a); k < count && record (k).time <= b; ++k)
            {
                const auto& dr = record (k).channel[ch];
                h.take (dr.tract * tr.weight, dr.r, dr.g, dr.b);
            }
            auto& gl = glow[j];
            gl.level = brightness (h.x);
            brightest = std::max (brightest, gl.level);
            gl.r = h.r;
            gl.g = h.g;
            gl.b = h.b;
            normalise (gl.r, gl.g, gl.b);
        }
    }

    // ---- Landing spots of the ascending pathway ----------------------------------------------
    for (auto& h : nodeHeld)
        decay (h.x, regionDecay);
    for (auto& side : hgHeld)
        for (auto& v : side)
            decay (v, hgDecay);
    for (auto& side : cochHeld)
        for (auto& v : side)
            decay (v, cochDecay);
    for (auto& v : earHeld)
        decay (v, cochDecay);
    const auto readRegion = [this] (double delay, auto&& take)
    {
        for (int k = firstAfter (previous - delay); k < count && record (k).time <= now - delay; ++k)
            take (record (k));
    };
    for (int side = kLeft; side <= kRight; ++side)
    {
        const auto s = static_cast<size_t> (side);
        const struct
        {
            Station station;
            Channel channel;
        } spots[] = { { Station::CochlearNucleus, Channel::EarAll }, { Station::Mso, Channel::OliveLow }, { Station::Lso, Channel::OliveHigh },
                      { Station::LateralLemniscus, Channel::AscendAll }, { Station::InferiorColliculus, Channel::AscendAll },
                      { Station::Mgn, Channel::AscendAll } };
        for (const auto& spot : spots)
        {
            auto& h = nodeHeld[static_cast<size_t> (node (spot.station, side))];
            const auto ch = static_cast<size_t> (channelIndex (spot.channel, side));
            readRegion (BrainAnatomy::stationDelay (spot.station), [&h, ch] (const Record& rec)
                        { h.take (rec.channel[ch].region, rec.channel[ch].r, rec.channel[ch].g, rec.channel[ch].b); });
        }
        readRegion (BrainAnatomy::stationDelay (Station::Heschl), [this, s] (const Record& rec)
                    {
                        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
                            hgHeld[s][b] = std::max (hgHeld[s][b], rec.hg[s][b]);
                    });
        readRegion (BrainAnatomy::stationDelay (Station::Cochlea), [this, s] (const Record& rec)
                    {
                        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
                            cochHeld[s][b] = std::max (cochHeld[s][b], rec.coch[s][b]);
                    });
        const auto earCh = static_cast<size_t> (channelIndex (Channel::EarAll, side));
        readRegion (0.0, [this, s, earCh] (const Record& rec) { earHeld[s] = std::max (earHeld[s], rec.channel[earCh].region); });
    }
    // Heschl's gyrus as one node (its strongest band) and the cochlea's (for hover and tests).
    for (int side = kLeft; side <= kRight; ++side)
    {
        const auto s = static_cast<size_t> (side);
        for (const Station st : { Station::Heschl, Station::Cochlea })
        {
            const auto& held = st == Station::Heschl ? hgHeld[s] : cochHeld[s];
            int best = 0;
            for (int b = 1; b < kBands; ++b)
                if (held[static_cast<size_t> (b)] > held[static_cast<size_t> (best)])
                    best = b;
            const auto rgb = frequencyRgb (bandHz (best));
            auto& h = nodeHeld[static_cast<size_t> (node (st, side))];
            h = Held {};
            h.take (held[static_cast<size_t> (best)], rgb[0], rgb[1], rgb[2]);
        }
        auto& ear = nodeHeld[static_cast<size_t> (node (Station::Ear, side))];
        ear = Held {};
        ear.take (earHeld[s], 0.62f, 0.68f, 0.85f);
    }

    // ---- Event waves ---------------------------------------------------------------------------
    for (size_t ti = 0; ti < tracts.size(); ++ti)
        if (tracts[ti].kind != TractKind::Auditory)
            std::fill_n (eventAccum.begin() + static_cast<std::ptrdiff_t> (ti * kPoints), kPoints, std::array<float, 4> {});
    for (auto& w : waves)
    {
        if (! w.active)
            continue;
        const double age = now - w.start;
        if (age < 0.0)
            continue;
        const float h = static_cast<float> (age / w.duration);
        const float fade = h > 1.0f ? static_cast<float> (std::exp (-(age - w.duration) / kWaveFadeSeconds)) : 1.0f;
        if (fade < 0.03f)
        {
            w.active = false;
            continue;
        }
        const auto& tr = tracts[static_cast<size_t> (w.tract)];
        if (h >= 1.0f && ! w.hit)
        {
            w.hit = true;
            hitNode (tr.to, w.amplitude, w.rgb);
        }
        const float head = std::min (h, 1.0f);
        auto* acc = &eventAccum[static_cast<size_t> (w.tract) * kPoints];
        for (int j = 0; j < kPoints; ++j)
        {
            const float u = static_cast<float> (j) / BrainAnatomy::kSegments;
            if (u > head)
                break;
            const float q = w.amplitude * fade * std::exp (-(head - u) / kWaveTrail);
            auto& a = acc[j];
            a[0] += w.rgb[0] * q;
            a[1] += w.rgb[1] * q;
            a[2] += w.rgb[2] * q;
            a[3] += q;
        }
    }
    for (size_t ti = 0; ti < tracts.size(); ++ti)
    {
        if (tracts[ti].kind == TractKind::Auditory)
            continue;
        for (size_t j = 0; j < static_cast<size_t> (kPoints); ++j)
        {
            const auto& a = eventAccum[ti * kPoints + j];
            auto& gl = segments[ti * kPoints + j];
            gl.level = brightness (a[3]);
            brightest = std::max (brightest, gl.level);
            gl.r = a[0];
            gl.g = a[1];
            gl.b = a[2];
            normalise (gl.r, gl.g, gl.b);
        }
    }

    // ---- Node display ------------------------------------------------------------------------
    for (size_t n = 0; n < nodeHeld.size(); ++n)
    {
        const auto& h = nodeHeld[n];
        auto& gl = nodeGlow[n];
        gl.level = brightness (h.x);
        brightest = std::max (brightest, gl.level);
        gl.r = h.r;
        gl.g = h.g;
        gl.b = h.b;
        normalise (gl.r, gl.g, gl.b);
    }
    dark = brightest <= 0.0f;
}
} // namespace flub::app::ui::vis::brain
