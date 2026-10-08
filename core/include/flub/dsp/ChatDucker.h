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
//             leading edge); intersample peaks are the master limiter's. The
//             limiter's state is its depth (1 - gain), so its release reaches
//             0 dB and the stage idles (as a gain, 1 - r (1 - g) stalled in
//             float about 2e-4 under 1: -0.0019 dB for ever at 48 kHz).
//   room      Game only, the chat sub-limiter (docs/11 E22 (1), 2026-10-08):
//             a gain after that limiter that leaves room for the chat. Per
//             sample, after the strip's gain g (strip gain x ChatMix), the
//             room's reference is the master ceiling over the device
//             correction's largest gain (Control::roomCeiling) or, where it
//             is higher, the offset ceiling x g (the strip turned up, or a
//             correction boosting more than the offset). The strip's peaks
//             x g may reach that reference minus amount x the Chat strip's
//             share of the sum (ChatRoomEnvelope, below), but never less
//             than the room floor under the reference (Control::roomFloorDb,
//             kDefaultRoomFloorDb). A loud teammate over a -1 dBTP explosion
//             then fits under the master ceiling, so the master limiter does
//             not turn the voice down; with the strip turned up the chat at
//             least adds nothing to what the master limiter takes off the
//             game. The strip's peak after the limiter, relative to the
//             offset ceiling, is held (instant attack, kRoomHoldMs: longer
//             than a 20 Hz period, re-armed by a peak within 0.1 % of it,
//             then kLimiterReleaseMs), and the gain scales that held peak
//             onto the room: so a chat that gets louder turns the strip down
//             smoothly instead of pinning its waveform to a falling ceiling
//             (a corner, which reads as a click on a steady low tone), and
//             only louder game peaks are caught at once. The room's floor
//             and its threshold (where it starts to turn the strip down) are
//             quadratic soft knees, so the gain has no corner where either
//             starts: the floor's 10 % of the floor's depth wide (the room
//             stays above the floor), the threshold's half-width 25 % of
//             how far the chat has taken the room down relative to the held
//             peak, at most 0.25 of it (smoothly: the gain stays in
//             0.75 .. 1 inside the knee; it turns the strip down a little
//             earlier, never less than the room needs). A
//             silent chat (its level 0: released, or under -100 dBFS)
//             leaves the offset ceiling alone, exactly. A muted strip (gain
//             under kMinPostGain) is not touched by the room. Floor 0 dB:
//             no room (the offset ceiling alone, as before 2026-10-08).
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
#include <vector>

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
    static constexpr float kHoldMs = 20.0f, kLimiterReleaseMs = 150.0f, kRoomHoldMs = 50.0f;
    /** The room (Game, see above): how far under its reference (the master
        ceiling over the correction's largest gain, or the offset ceiling
        after the strip's gain where that is higher) it may push the strip's
        peaks after its gain at most (0 dB: no room; clamped to
        kMinRoomFloorDb .. 0), and the strip gain under which the strip
        adds nothing and the room leaves it alone. */
    static constexpr float kDefaultRoomFloorDb = -6.0f, kMinRoomFloorDb = -24.0f;
    static constexpr float kMinPostGain = 1.0e-4f;
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
        // Game, the room (see above); chatLevel nullptr: no room.
        const float* chatLevel = nullptr;  // the block's ChatRoomEnvelope (the Chat strip's share of the sum), per sample
        float postGain0 = 1.0f;            // the strip's gain after this stage (strip gain x ChatMix) at the block's
        float postGain1 = 1.0f;            //   first sample and after its last (a linear glide, as MixEngine sums it)
        float roomCeiling = 1.0f;          // linear: the master ceiling over the device correction's largest gain
        float roomFloorDb = kDefaultRoomFloorDb; // the room's floor under its reference (>= 0: no room)
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
    /** The deepest gain of the ceiling limiter and the room together in
        the last block (dB <= 0); any thread. */
    float getCeilingGainDb() const noexcept { return ceilingGainDb.load (std::memory_order_relaxed); }
    /** The room's deepest level in the last block where it turned the
        strip down, in dB under the room's reference at that sample (see
        above; >= the floor): < 0 whenever the room acted, 0 when the
        offset ceiling alone held. Any thread. */
    float getRoomDb() const noexcept { return roomDb.load (std::memory_order_relaxed); }
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
    float limiterDepth = 0.0f, limiterRelease = 0.0f; // depth = 1 - the limiter's gain
    int holdSamples = 0, holdLeft = 0;
    // The room (Game): the limited output's held peak re the ceiling.
    float roomPeak = 0.0f;
    int roomHoldSamples = 0, roomHoldLeft = 0;
    std::atomic<float> ceilingGainDb { 0.0f }, roomDb { 0.0f };
};

/** The Chat strip's share of the sum for the room (docs/11 E22, see
    ChatDucker above), one value per sample: the stereo-linked magnitude of
    the Chat strip's output times its gain, shaped so that the room it
    leaves never steps or bends sharply. A backward pass over the block
    looks ahead: each peak is held kLeadMs before it, and before that the
    level rises towards it by at most full scale per kAttackMs, so it
    reaches a peak of level A on time when the peak lies at least
    kLeadMs + A x kAttackMs inside the block; across blocks the rise is
    rate-limited by the same slope (a peak at the start of a block is
    reached up to A x kAttackMs late; the master limiter holds that
    moment). After a peak the level is held kHoldMs (longer than a voice's
    pitch period, so the room does not open between glottal pulses) and
    released over kReleaseMs. A one-pole of kSmoothMs then rounds the
    ramps' corners (its lag is what kLeadMs covers: within 0.3 % at the
    peak), so the room's gain has no corner. The 3 ms slope: a stepped
    level clicks on a steady low tone held at the offset ceiling (the
    chat jumping to 0.7, floor -12 dB); this envelope does not, also
    where it cannot look ahead. Non-finite samples and levels under
    kSilentLevel (-100 dBFS) read as 0, so a released chat gives exactly
    0 (the room then leaves the offset ceiling alone). MixEngine runs one
    for the Chat strip while a Game strip's duck is in. */
class ChatRoomEnvelope
{
public:
    static constexpr float kAttackMs = 3.0f, kLeadMs = 1.0f, kSmoothMs = 0.25f, kHoldMs = 20.0f, kReleaseMs = 150.0f;
    static constexpr float kSilentLevel = 1.0e-5f;

    /** Non-RT: allocates the block buffer; resets. */
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept FLUB_NONBLOCKING;
    /** The Chat strip's stereo output before its gain (nullptr: silence) and
        that gain at the block's first sample and after its last (linear
        glide); numSamples <= the prepared maximum. Returns the levels. */
    const float* process (const AudioBlock* chat, int numSamples, float gain0, float gain1) noexcept FLUB_NONBLOCKING;
    /** The level after the last sample processed. */
    float getLevel() const noexcept { return smoothed; }

private:
    std::vector<float> levels;
    float slope = 1.0f, release = 0.0f, smoothing = 0.0f, level = 0.0f, smoothed = 0.0f;
    int leadSamples = 1, holdSamples = 1, holdLeft = 0;
};
} // namespace flub
