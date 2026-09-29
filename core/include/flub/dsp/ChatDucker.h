// Flubsound Pro - the voice-keyed duck on the Game and Music strips (docs/11 E22).
//
// While VoiceActivity hears a teammate on the Chat strip, MixEngine runs each
// Game and Music strip's output (after its chain, before the sum) through
// one of these, so the callout is not masked and the master limiter is not
// made to duck the chat on every explosion:
//   amount    0..1, towards 1 while the voice is active (one-pole, attack
//             kAttackMs) and back to 0 when it is not (release kReleaseMs);
//             every section below scales with it.
//   dip       Music: a bell at 2 kHz, Q 0.7, -depth (about -depth / 2 at 1
//             and 4 kHz). Game: the same 1 - 4 kHz duck without touching
//             the footstep detail band (the Gaming cue band at 3.2 kHz,
//             ProcessingChain's mode band 4): a bell at 1.2 kHz, Q 1.2,
//             -depth, one at 2.1 kHz, Q 3, -0.8 depth, and one at 3 kHz,
//             Q 1.4, +0.3 depth, which takes back the first two's skirts
//             above 2.6 kHz. At depth 3 / 6 dB it is -2.5 / -5.1 dB at 1 kHz,
//             at least -2.5 / -5.0 dB over 1.25 - 2.2 kHz, -1.5 / -3.0 dB at
//             2.4 kHz, and within +-0.5 dB over 2.8 - 6 kHz (the protected
//             region) and below 300 Hz (the footstep body band).
//   lift      Game only: the chain's Voice & Score lift (Gaming mode band
//             7: a BoostBelow bell at kVoiceLiftHz, Q kVoiceLiftQ, the
//             Voice macro's), read from the Game chain's MeterBus, taken back
//             by the exact inverse bell (an SVF bell of -g inverts one of
//             +g at the same frequency and Q) while chat is active, so the
//             game's own 2 kHz lift does not compete with the callout. It is
//             one block late and sits after the chain's dynamics, so it
//             cancels the lift as the chain applied it to within the chain's
//             own gain changes (tests/test_mix_engine_sidechain.cpp).
//   ceiling   Game only: strip-priority master protection. The strip's peaks
//             are held kCeilingOffsetDb x amount under the master ceiling by
//             a zero-latency peak limiter (stereo-linked sample peak,
//             instant attack, kHoldMs hold, kLimiterReleaseMs release), so a
//             -1 dBTP explosion leaves the master limiter room for the voice
//             instead of ducking the whole sum. With no look-ahead the first
//             half-cycle over the ceiling is flattened (the explosion's
//             leading edge); intersample peaks are the master limiter's.
// Sections are SVFs (modulation-safe), their gains recomputed every
// kUpdateSamples. At amount 0, with the limiter released, the stage idles
// and does not touch the signal (bit-exact); the next start begins from
// cleared filters. Latency 0.
//
// Threading: prepare() before the audio runs; the rest on the audio thread.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Svf.h"

#include <array>
#include <atomic>

namespace flub
{
class ChatDucker
{
public:
    enum class Shape
    {
        Music, // the 2 kHz bell only
        Game   // footstep-protected dip, Voice & Score lift cancel, ceiling offset
    };

    static constexpr float kAttackMs = 30.0f, kReleaseMs = 300.0f;
    static constexpr float kMinDepthDb = 3.0f, kMaxDepthDb = 6.0f, kDefaultDepthDb = 4.5f;
    static constexpr float kCeilingOffsetDb = 3.0f;
    static constexpr float kHoldMs = 20.0f, kLimiterReleaseMs = 150.0f;
    /** The Gaming Voice & Score band (ProcessingChain's mode band 7). */
    static constexpr float kVoiceLiftHz = 2000.0f, kVoiceLiftQ = 0.7f;
    static constexpr int kUpdateSamples = 16;

    /** Non-RT (or before the strip runs). */
    void prepare (double sampleRate, Shape shape) noexcept;
    /** Clears the filters and the limiter; the duck starts at `amount`. */
    void reset (float amount = 0.0f) noexcept FLUB_NONBLOCKING;

    struct Control
    {
        bool voiceActive = false;
        float depthDb = kDefaultDepthDb;   // clamped to [kMinDepthDb, kMaxDepthDb]
        float liftDb = 0.0f;               // Game: the chain's Voice & Score lift now (>= 0)
        float ceilingDb = -1.0f;           // Game: the master ceiling, dBFS
    };

    /** The strip's stereo output, in place. */
    void process (const AudioBlock& stereo, const Control& control) noexcept FLUB_NONBLOCKING;
    /** A frozen strip (MixEngine idle freeze): the amount and the limiter
        move on over `numSamples` as if silence had been processed. */
    void skip (int numSamples, bool voiceActive) noexcept FLUB_NONBLOCKING;

    /** 0..1 now. */
    float getAmount() const noexcept { return amount; }
    /** The dip applied now at its deepest point (dB <= 0). */
    float getDipDb() const noexcept { return -appliedDepthDb * amount; }
    /** The ceiling limiter's deepest gain in the last block (dB <= 0); any thread. */
    float getCeilingGainDb() const noexcept { return ceilingGainDb.load (std::memory_order_relaxed); }
    /** The lift cancel's gain now (dB <= 0). */
    float getLiftCancelDb() const noexcept { return liftCancelDb; }
    bool isIdle() const noexcept { return idle; }

    /** The dip's magnitude at `hz` for `depthDb` (the full amount), from the
        sections' exact responses: for tests and docs. */
    static double dipResponseDb (Shape shape, double depthDb, double hz, double sampleRate) noexcept;

private:
    static constexpr int kMaxSections = 4;
    void design (float depthDb, float liftDb) noexcept FLUB_NONBLOCKING;
    void stepAmount (bool voiceActive, int numSamples) noexcept FLUB_NONBLOCKING;

    double sampleRate = 48000.0;
    Shape shape = Shape::Music;
    std::array<SvfCoeffs, kMaxSections> coeffs {};
    std::array<std::array<SvfState, kMaxSections>, 2> states {};
    int numSections = 1;
    float amount = 0.0f, appliedDepthDb = kDefaultDepthDb, liftCancelDb = 0.0f;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    int countdown = 0;
    bool idle = true;

    // The zero-latency ceiling limiter (Game).
    float limiterGain = 1.0f, limiterRelease = 0.0f;
    int holdSamples = 0, holdLeft = 0;
    std::atomic<float> ceilingGainDb { 0.0f };
};
} // namespace flub
