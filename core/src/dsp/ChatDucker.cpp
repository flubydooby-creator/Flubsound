#include "flub/dsp/ChatDucker.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace flub
{
namespace
{
/** One dip section: frequency, Q and its gain as a share of the depth. */
struct Section
{
    double hz, q, share;
};

// The Game dip (see the header): a 1 - 2.4 kHz duck whose skirts are taken
// back above 2.6 kHz, so the footstep detail band is left within 0.5 dB.
constexpr Section kGameDip[] = { { 1200.0, 1.2, -1.0 }, { 2100.0, 3.0, -0.8 }, { 3000.0, 1.4, 0.3 } };
constexpr Section kMusicDip[] = { { 2000.0, 0.7, -1.0 } };
constexpr float kIdleAmount = 1.0e-3f; // the dip then is under 0.01 dB

template <size_t N>
void designSections (const Section (&dip)[N], double depthDb, double sampleRate, SvfCoeffs* out) noexcept
{
    for (size_t i = 0; i < N; ++i)
        out[i] = SvfCoeffs::make (FilterType::Bell, dip[i].hz, dip[i].q, dip[i].share * depthDb, sampleRate);
}
} // namespace

void ChatDucker::prepare (double sr, Shape s) noexcept
{
    sampleRate = sr;
    shape = s;
    numSections = shape == Shape::Game ? static_cast<int> (std::size (kGameDip)) + 1 : static_cast<int> (std::size (kMusicDip));
    attackCoeff = onePoleCoeff (kAttackMs, sr / kUpdateSamples);
    releaseCoeff = onePoleCoeff (kReleaseMs, sr / kUpdateSamples);
    limiterRelease = onePoleCoeff (kLimiterReleaseMs, sr);
    holdSamples = std::max (1, static_cast<int> (std::lround (kHoldMs * 0.001 * sr)));
    reset();
}

void ChatDucker::reset (float startAmount) noexcept FLUB_NONBLOCKING
{
    for (auto& channel : states)
        for (auto& s : channel)
            s.reset();
    amount = std::clamp (startAmount, 0.0f, 1.0f);
    liftCancelDb = 0.0f;
    limiterDepth = 0.0f;
    holdLeft = 0;
    countdown = 0;
    idle = amount <= 0.0f;
    ceilingGainDb.store (0.0f, std::memory_order_relaxed);
    roomDb.store (0.0f, std::memory_order_relaxed);
    design (appliedDepthDb, 0.0f);
}

void ChatDucker::stepAmount (bool voiceActive, int numSamples) noexcept FLUB_NONBLOCKING
{
    // Per kUpdateSamples step (the coefficients are per step).
    const float target = voiceActive ? 1.0f : 0.0f;
    for (int left = numSamples; left > 0; left -= kUpdateSamples)
    {
        const float c = target > amount ? attackCoeff : releaseCoeff;
        amount = target + c * (amount - target);
    }
    if (! voiceActive && amount < kIdleAmount)
        amount = 0.0f;
}

void ChatDucker::design (float depthDb, float liftDb) noexcept FLUB_NONBLOCKING
{
    const double d = static_cast<double> (depthDb) * static_cast<double> (amount);
    if (shape == Shape::Game)
    {
        designSections (kGameDip, d, sampleRate, coeffs.data());
        liftCancelDb = -std::max (0.0f, liftDb) * amount;
        coeffs[std::size (kGameDip)] = SvfCoeffs::make (FilterType::Bell, kVoiceLiftHz, kVoiceLiftQ, liftCancelDb, sampleRate);
    }
    else
        designSections (kMusicDip, d, sampleRate, coeffs.data());
}

void ChatDucker::process (const AudioBlock& stereo, const Control& control) noexcept FLUB_NONBLOCKING
{
    const int n = stereo.numSamples;
    if (idle && ! control.voiceActive)
    {
        ceilingGainDb.store (0.0f, std::memory_order_relaxed);
        roomDb.store (0.0f, std::memory_order_relaxed);
        return;
    }
    if (idle)
    {
        // A new start: from cleared filters (the dip starts from 0 dB).
        for (auto& channel : states)
            for (auto& s : channel)
                s.reset();
        idle = false;
        countdown = 0;
    }
    appliedDepthDb = std::clamp (control.depthDb, kMinDepthDb, kMaxDepthDb);
    const bool game = shape == Shape::Game;
    float* ch[2] = { stereo.channel (0), stereo.channel (stereo.numChannels > 1 ? 1 : 0) };
    const int channels = stereo.numChannels > 1 ? 2 : 1;
    float deepest = 1.0f, deepestRoom = 1.0f;
    // The room (Game): a chat level is given, the floor is under 0 dB and
    // the ceiling it is measured from is usable.
    const bool room = game && control.chatLevel != nullptr && control.roomFloorDb < 0.0f && std::isfinite (control.roomCeiling)
                      && control.roomCeiling > 0.0f && std::isfinite (control.postGain0) && std::isfinite (control.postGain1);
    const float roomFloor = room ? control.roomCeiling * dbToGain (std::max (control.roomFloorDb, kMinRoomFloorDb)) : 0.0f;
    const float gainStep = room ? (control.postGain1 - control.postGain0) / static_cast<float> (std::max (1, n)) : 0.0f;

    for (int pos = 0; pos < n;)
    {
        if (countdown == 0)
        {
            stepAmount (control.voiceActive, kUpdateSamples);
            design (appliedDepthDb, control.liftDb);
            countdown = kUpdateSamples;
        }
        const int len = std::min (n - pos, countdown);
        for (int c = 0; c < channels; ++c)
        {
            float* x = ch[c] + pos;
            auto& st = states[static_cast<size_t> (c)];
            for (int k = 0; k < len; ++k)
            {
                float y = x[k];
                for (int s = 0; s < numSections; ++s)
                    y = svfTick (coeffs[static_cast<size_t> (s)], st[static_cast<size_t> (s)], y);
                x[k] = y;
            }
        }
        if (game)
        {
            // The ceiling offset: a zero-latency, stereo-linked peak limiter.
            const float ceiling = dbToGain (control.ceilingDb - kCeilingOffsetDb * amount);
            float* l = ch[0] + pos;
            float* r = ch[channels - 1] + pos;
            for (int k = 0; k < len; ++k)
            {
                // The room: what the chat leaves of the master ceiling (after
                // the strip's gain; a muted strip is left alone), never under
                // the floor; the lower of it and the offset ceiling holds.
                float limit = ceiling;
                if (room)
                {
                    const float g = control.postGain0 + gainStep * static_cast<float> (pos + k);
                    if (g > kMinPostGain)
                    {
                        const float post = std::max (roomFloor, control.roomCeiling - amount * control.chatLevel[pos + k]);
                        if (post < limit * g)
                        {
                            limit = post / g;
                            deepestRoom = std::min (deepestRoom, post / control.roomCeiling);
                        }
                    }
                }
                const float peak = std::max (std::abs (l[k]), std::abs (r[k]));
                const float target = peak > limit ? limit / peak : 1.0f;
                // The state is the depth (1 - gain): released as 1 - r (1 - g)
                // the gain stalled in float about 2e-4 under 1 at 48 kHz (the
                // step fell under half an ulp) and the stage never idled; the
                // depth itself decays all the way.
                if (target < 1.0f - limiterDepth)
                {
                    limiterDepth = 1.0f - target;
                    holdLeft = holdSamples;
                }
                else if (holdLeft > 0)
                    --holdLeft;
                else if (limiterDepth > 0.0f)
                {
                    limiterDepth = std::max (1.0f - target, limiterRelease * limiterDepth);
                    if (limiterDepth < 1.0e-6f)
                        limiterDepth = 0.0f;
                }
                const float gain = 1.0f - limiterDepth;
                if (limiterDepth > 0.0f)
                {
                    l[k] *= gain;
                    if (channels > 1)
                        r[k] *= gain;
                }
                deepest = std::min (deepest, gain);
            }
        }
        pos += len;
        countdown -= len;
    }
    ceilingGainDb.store (deepest < 1.0f ? gainToDb (deepest) : 0.0f, std::memory_order_relaxed);
    roomDb.store (deepestRoom < 1.0f ? gainToDb (deepestRoom) : 0.0f, std::memory_order_relaxed);
    if (amount <= 0.0f && ! control.voiceActive && limiterDepth <= 0.0f)
    {
        idle = true;
        liftCancelDb = 0.0f;
    }
}

void ChatDucker::skip (int numSamples, bool voiceActive) noexcept FLUB_NONBLOCKING
{
    if (idle && ! voiceActive)
        return;
    stepAmount (voiceActive, numSamples);
    countdown = 0;
    // Silence went through: the limiter has released.
    limiterDepth = 0.0f;
    holdLeft = 0;
    ceilingGainDb.store (0.0f, std::memory_order_relaxed);
    roomDb.store (0.0f, std::memory_order_relaxed);
    if (amount <= 0.0f && ! voiceActive)
    {
        idle = true;
        liftCancelDb = 0.0f;
    }
}

double ChatDucker::dipResponseDb (Shape s, double depthDb, double hz, double sr) noexcept
{
    std::array<SvfCoeffs, kMaxSections> c {};
    std::complex<double> h = 1.0;
    if (s == Shape::Game)
    {
        designSections (kGameDip, depthDb, sr, c.data());
        for (size_t i = 0; i < std::size (kGameDip); ++i)
            h *= c[i].response (hz, sr);
    }
    else
    {
        designSections (kMusicDip, depthDb, sr, c.data());
        h *= c[0].response (hz, sr);
    }
    return 20.0 * std::log10 (std::max (1.0e-12, std::abs (h)));
}

// =============================================================================
// ChatRoomEnvelope
// =============================================================================
void ChatRoomEnvelope::prepare (double sr, int maxBlockSize)
{
    levels.assign (static_cast<size_t> (std::max (1, maxBlockSize)), 0.0f);
    slope = static_cast<float> (1.0 / std::max (1.0, static_cast<double> (kAttackMs) * 0.001 * sr));
    release = onePoleCoeff (kReleaseMs, sr);
    holdSamples = std::max (1, static_cast<int> (std::lround (kHoldMs * 0.001 * sr)));
    reset();
}

void ChatRoomEnvelope::reset() noexcept FLUB_NONBLOCKING
{
    level = 0.0f;
    holdLeft = 0;
}

const float* ChatRoomEnvelope::process (const AudioBlock* chat, int numSamples, float gain0, float gain1) noexcept FLUB_NONBLOCKING
{
    const int n = std::clamp (numSamples, 0, static_cast<int> (levels.size()));
    float* h = levels.data();
    // The Chat strip's share of the sum, stereo-linked, through its gain as
    // MixEngine glides it; a NaN reads as 0, anything huge as kMaxLevel.
    constexpr float kMaxLevel = 16.0f;
    if (chat != nullptr && chat->numChannels > 0 && chat->numSamples >= n)
    {
        const float* l = chat->channel (0);
        const float* r = chat->channel (chat->numChannels > 1 ? 1 : 0);
        const float step = (gain1 - gain0) / static_cast<float> (std::max (1, n));
        for (int k = 0; k < n; ++k)
        {
            const float v = std::abs (gain0 + step * static_cast<float> (k)) * std::max (std::abs (l[k]), std::abs (r[k]));
            h[k] = v < kMaxLevel ? v : (v >= kMaxLevel ? kMaxLevel : 0.0f);
        }
    }
    else
        std::fill (h, h + n, 0.0f);

    // Look ahead within the block: towards each peak the level rises by at
    // most `slope` per sample, so it is there when the peak is.
    for (int k = n - 2; k >= 0; --k)
        h[k] = std::max (h[k], h[k + 1] - slope);

    // Across blocks the same slope; after a peak hold, then release.
    for (int k = 0; k < n; ++k)
    {
        const float t = h[k];
        if (t > level)
        {
            level = std::min (t, level + slope);
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
            --holdLeft;
        else
        {
            level = t + release * (level - t);
            if (level < 1.0e-9f)
                level = 0.0f;
        }
        h[k] = level;
    }
    return h;
}
} // namespace flub
