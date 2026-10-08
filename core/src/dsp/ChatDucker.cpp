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

// The room's two soft knees (see process): the threshold's half-width as a
// share of how far the chat has taken the room down (relative to the held
// peak), at most kRoomKneeMax (smoothly), the floor's as a share of the
// floor's depth.
constexpr float kRoomKnee = 0.25f, kRoomKneeMax = 0.25f, kRoomFloorKnee = 0.1f;

/** max (0, z) with a quadratic knee of half-width w > 0: C1, never under it. */
inline float softMax0 (float z, float w) noexcept
{
    return z >= w ? z : (z <= -w ? 0.0f : (z + w) * (z + w) / (4.0f * w));
}

/** min (1, u) with a quadratic knee of half-width w >= 0: C1 for w > 0,
    never over it. */
inline float softMin1 (float u, float w) noexcept
{
    if (u <= 1.0f - w)
        return u;
    if (u >= 1.0f + w)
        return 1.0f;
    return u - (u - 1.0f + w) * (u - 1.0f + w) / (4.0f * w);
}

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
    roomHoldSamples = std::max (1, static_cast<int> (std::lround (kRoomHoldMs * 0.001 * sr)));
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
    roomPeak = 0.0f;
    roomHoldLeft = 0;
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
        roomPeak = 0.0f;
        roomHoldLeft = 0;
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
    const float roomFloor = room ? dbToGain (std::max (control.roomFloorDb, kMinRoomFloorDb)) : 0.0f; // re the room's reference
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
            const float perCeiling = 1.0f / ceiling;
            float* l = ch[0] + pos;
            float* r = ch[channels - 1] + pos;
            for (int k = 0; k < len; ++k)
            {
                const float peak = std::max (std::abs (l[k]), std::abs (r[k]));
                const float target = peak > ceiling ? ceiling / peak : 1.0f;
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
                float gain = 1.0f - limiterDepth;
                if (room)
                {
                    // The room, a gain on the limited output. Its peak
                    // relative to the ceiling (<= 1) is held (instant attack,
                    // kRoomHoldMs, re-armed by a peak within 0.1 % of it, then
                    // kLimiterReleaseMs); where the room is under the offset
                    // ceiling the gain scales that held peak onto the room.
                    // So a chat that gets louder turns the strip down as
                    // smoothly as its level moves (it never pins the waveform
                    // to a falling ceiling, which leaves a corner), and only
                    // louder game peaks are caught at once, as the offset
                    // ceiling catches them. The hold outlasts a 20 Hz period,
                    // so a steady low tone never releases between its peaks
                    // (the next peak would catch the released hold on its
                    // rising edge: a corner).
                    const float relative = peak * gain * perCeiling;
                    if (relative >= roomPeak * 0.999f)
                    {
                        roomPeak = std::max (roomPeak, relative);
                        roomHoldLeft = roomHoldSamples;
                    }
                    else if (roomHoldLeft > 0)
                        --roomHoldLeft;
                    else
                        roomPeak = std::max (relative, limiterRelease * roomPeak);
                    // After the strip's gain (a muted strip is left alone)
                    // the sum may reach the room's reference: the master
                    // ceiling over the correction's largest gain or, where
                    // that is higher (the strip turned up, a correction
                    // boosting more than the offset), the offset ceiling
                    // after the gain. The chat's share comes off it, never
                    // more than the floor; a silent chat (share 0) leaves
                    // the offset ceiling alone, exactly.
                    const float g = control.postGain0 + gainStep * static_cast<float> (pos + k);
                    const float share = amount * control.chatLevel[pos + k];
                    if (g > kMinPostGain && share > 0.0f && roomPeak > 0.0f)
                    {
                        const float offsetPost = ceiling * g;
                        const float reference = std::max (control.roomCeiling, offsetPost);
                        // The floor through a soft knee (post never under it).
                        const float floorLevel = reference * roomFloor;
                        const float post = floorLevel + softMax0 (reference - share - floorLevel, kRoomFloorKnee * (reference - floorLevel));
                        // The room re the held peak, u (< 1: the strip comes
                        // down), through a soft knee at 1 whose half-width
                        // grows with how far the chat has taken u down, up
                        // to kRoomKneeMax: the gain is C1 in the chat's level
                        // (a hard min would put a corner where the room
                        // starts to act), never above u, 1 while the chat is
                        // silent, and in 0.75 .. 1 inside the knee (an
                        // unbounded knee reached below 0 with a quiet game,
                        // a deep floor and a loud chat).
                        const float held = offsetPost * roomPeak;
                        const float spread = kRoomKnee * (reference - post) / held;
                        const float roomGain = softMin1 (post / held, spread / (1.0f + spread / kRoomKneeMax));
                        if (roomGain < 1.0f)
                        {
                            gain *= roomGain;
                            deepestRoom = std::min (deepestRoom, post / reference);
                        }
                    }
                }
                if (gain < 1.0f)
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
    roomPeak = 0.0f;
    roomHoldLeft = 0;
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
    smoothing = onePoleCoeff (kSmoothMs, sr);
    leadSamples = std::max (1, static_cast<int> (std::lround (kLeadMs * 0.001 * sr)));
    // Held kHoldMs after the peak: the hold starts where the lead does.
    holdSamples = std::max (1, static_cast<int> (std::lround (kHoldMs * 0.001 * sr))) + leadSamples;
    reset();
}

void ChatRoomEnvelope::reset() noexcept FLUB_NONBLOCKING
{
    level = 0.0f;
    smoothed = 0.0f;
    holdLeft = 0;
}

const float* ChatRoomEnvelope::process (const AudioBlock* chat, int numSamples, float gain0, float gain1) noexcept FLUB_NONBLOCKING
{
    const int n = std::clamp (numSamples, 0, static_cast<int> (levels.size()));
    float* h = levels.data();
    // The Chat strip's share of the sum, stereo-linked, through its gain as
    // MixEngine glides it; a NaN or a level under kSilentLevel reads as 0,
    // anything huge as kMaxLevel.
    constexpr float kMaxLevel = 16.0f;
    if (chat != nullptr && chat->numChannels > 0 && chat->numSamples >= n)
    {
        const float* l = chat->channel (0);
        const float* r = chat->channel (chat->numChannels > 1 ? 1 : 0);
        const float step = (gain1 - gain0) / static_cast<float> (std::max (1, n));
        for (int k = 0; k < n; ++k)
        {
            const float v = std::abs (gain0 + step * static_cast<float> (k)) * std::max (std::abs (l[k]), std::abs (r[k]));
            h[k] = v < kMaxLevel ? (v >= kSilentLevel ? v : 0.0f) : (v >= kMaxLevel ? kMaxLevel : 0.0f);
        }
    }
    else
        std::fill (h, h + n, 0.0f);

    // Look ahead within the block: each peak is held kLeadMs before it (the
    // smoother's lag below), and before that the level rises towards it by
    // at most `slope` per sample, so it is there when the peak is.
    float ahead = 0.0f;
    int leadLeft = 0;
    for (int k = n - 1; k >= 0; --k)
    {
        if (leadLeft > 0)
            --leadLeft;
        else
            ahead = std::max (0.0f, ahead - slope);
        if (h[k] >= ahead)
        {
            ahead = h[k];
            leadLeft = leadSamples;
        }
        h[k] = ahead;
    }

    // Across blocks the same slope; after a peak hold, then release. The
    // smoother rounds the corners of these ramps (a level whose slope steps
    // would step the room's gain slope: a corner in the Game strip's
    // waveform).
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
            if (level < kSilentLevel)
                level = 0.0f;
        }
        smoothed = level + smoothing * (smoothed - level);
        if (level == 0.0f && smoothed < kSilentLevel)
            smoothed = 0.0f;
        h[k] = smoothed;
    }
    return h;
}
} // namespace flub
