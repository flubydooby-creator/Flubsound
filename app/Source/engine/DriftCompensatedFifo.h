// Flubsound Pro - clock-drift compensating FIFO for captured audio streams.
//
// A per-application loopback capture (ProcessLoopbackCapture) runs on its own
// thread, paced by the *capture* clock, while the engine is paced by the
// *output device* clock. Even at the same nominal rate the two clocks differ
// by tens to hundreds of ppm, so a plain FIFO slowly drains (clicks from
// underruns) or fills (growing latency, then overflow). This class hides that:
//
//   capture thread --push()--> SpscRing (interleaved) --pull()--> audio thread
//                                                    |
//                          band-limited fractional resampler (Kernel), ratio =
//                          nominal (producerRate / consumerRate) * (1 + c)
//                                                    |
//               PI controller: c = Kp * e + Ki * integral(e),
//               e = smoothed fill level - target (in seconds)
//
// * Target fill = 2 device blocks (in producer frames), raised automatically
//   to "largest recent producer burst + 1 device block" because capture APIs
//   deliver in packets (e.g. 10 ms) that can be larger than a device block.
// * The loop is critically damped with a natural frequency of 0.15 rad/s and
//   the correction is clamped to +-0.5 % (kMaxCorrection: +-5000 ppm, at most
//   ~8.7 cents of pitch shift while a large fill error is pulled in). In
//   steady state the correction equals the clock drift: typical drifts
//   (< 500 ppm, i.e. < 0.9 cent, inaudible) are absorbed without any dropout
//   and the correction ripple stays in the tens of ppm.
// * Underrun (not enough data for a block): the block fades from the last
//   output value to silence (no DC step), is counted, and the FIFO re-primes
//   until the fill is back at target; playback then resumes with a 5 ms fade-in.
// * Overflow (fill above a high-water mark, e.g. after the device stalled):
//   the consumer drops the OLDEST frames down to the target and counts it. If
//   the ring is completely full the producer has no choice but to drop the
//   newest frames; both are reported in Stats::droppedFrames.
// * Sanitising (docs/11 E10): push() mutes every NaN / Inf sample and every
//   finite one beyond +24 dBFS (ProcessingChain::kSanitiseLimit) and counts
//   it (Stats::corruptSamples), so one bad sample from one application stays
//   one silent sample of that stream: it never reaches the resampler's
//   history or the other streams mixed into the same block, where the
//   chain's own guard would have to drop or mute the whole mix. A clean
//   same-layout push is still a straight copy (one scan to check it). On
//   Linux, applications are mixed inside PipeWire before Flubsound sees
//   them, so a NaN from one application reaches every capture of that mix;
//   this guard (and the chain's) then limits it to the samples it hit.
//
// Threading: exactly one producer thread and one consumer thread.
//   prepare()             non-RT, neither side running
//   setConsumerFormat()   non-RT, consumer (audio callback) not running; the
//                         producer may keep pushing
//   push()                producer thread, RT-safe (no locks, no allocation)
//   pull() / restart()    consumer thread, RT-safe
//   getStats()            any thread (relaxed atomics)
//
// Resampler (docs/11 E50 Phase A): a polyphase Kaiser-windowed sinc
// (Kernel: kTaps taps, kPhases phases, linear interpolation between adjacent
// phases), the same for every FIFO. Its passband is flat within 0.001 dB to
// 20 kHz at 48 kHz at every fractional phase and its images sit >= 100 dB
// down; the 4-point Catmull-Rom interpolator it replaces lost up to 8.4 dB at
// 20 kHz (phase 0.5) and, with a nominal ratio of 1, either held a random,
// session-dependent top-octave shelf or swept it slowly with the drift. The
// kernel delays the stream by fixedDelayFrames() (reported in
// Stats::resamplerDelayMs and the host's LatencyInfo::captureBufferMs). The
// kernel's history is kept apart from the ring, so the ring's fill target
// (and with it the drift loop's set-point) is unchanged.
//
// Large nominal ratios (e.g. 96 kHz capture into a 48 kHz device) work, but
// the kernel's cutoff sits at the producer's Nyquist frequency, so a
// downward ratio is not band-limited to the consumer's; the host therefore requests
// captures at the device rate so the nominal ratio is 1, and when the device
// rate changes it restarts every running capture at the new rate (with a
// fresh prepare()) instead of calling setConsumerFormat() with a new rate
// (AudioEngineHost::restartCapturesAtDeviceRate).
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/common/SpscRing.h"
#include "flub/dsp/Bs775Fold.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace flub::app
{
class DriftCompensatedFifo
{
public:
    struct Stats
    {
        uint64_t underruns = 0;         // blocks rendered as silence for lack of data
        uint64_t overflows = 0;         // times the oldest data was dropped (fill too high)
        uint64_t droppedFrames = 0;     // producer frames discarded (overflow + ring full)
        uint64_t framesPushed = 0;      // total producer frames received
        uint64_t corruptSamples = 0;    // NaN / Inf / beyond +24 dBFS samples muted by push()
        float fillMs = 0.0f;            // smoothed fill level
        float targetMs = 0.0f;          // current target fill level
        float correctionPpm = 0.0f;     // PI controller output (+ = consuming faster)
        float resamplerDelayMs = 0.0f;  // the resampler's fixed delay (fixedDelayFrames at the producer rate)
        bool streaming = false;         // false while priming / after an underrun
    };

    /** docs/11 E50 Phase A: the fractional-delay kernel. Row p of the table
        (0 <= p <= kPhases) holds the kTaps coefficients for the fraction
        p / kPhases: a Kaiser-windowed sinc (cutoff at the producer's
        Nyquist frequency, beta kBeta) centred between window[kTaps/2 - 1]
        (fraction 0) and window[kTaps/2] (fraction 1), each row normalised to
        a DC gain of 1. Fraction 0 is the identity. */
    struct Kernel
    {
        static constexpr int kTaps = 48;
        static constexpr int kPhases = 512;
        static constexpr double kBeta = 11.0;

        /** The (kPhases + 1) x kTaps table, built on first use (thread-safe;
            prepare() builds it, never the audio thread). */
        static const float* table();

        /** The coefficients for `fraction` in [0, 1), linearly interpolated
            between the two nearest rows of `rows` (table()). RT-safe. */
        static void coefficients (const float* rows, float fraction, float* out) noexcept FLUB_NONBLOCKING;

        /** One output from kTaps input frames of one channel (oldest first):
            the sum of window[k] * coefficients[k]. RT-safe. */
        static float apply (const float* window, const float* coefficients) noexcept FLUB_NONBLOCKING
        {
            // Eight independent partial sums: the compiler can keep them in
            // vector registers (a single running sum would force the
            // additions into sequence).
            static_assert (kTaps % 8 == 0);
            std::array<float, 8> acc {};
            for (int k = 0; k < kTaps; k += 8)
                for (int j = 0; j < 8; ++j)
                    acc[static_cast<size_t> (j)] += window[k + j] * coefficients[k + j];
            return ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
        }
    };

    /** The resampler's delay in producer frames: kTaps / 2 - fraction, i.e.
        this value +-0.5 frame. */
    static constexpr double fixedDelayFrames() noexcept { return Kernel::kTaps / 2 - 0.5; }

    DriftCompensatedFifo() = default;

    /** Non-RT. Allocates everything. numChannels 1..kMaxChannels. */
    void prepare (int numChannels, double producerSampleRate, double consumerSampleRate, int consumerMaxBlockSize,
                  double bufferSeconds = 1.0);

    /** Non-RT, consumer stopped (the producer may keep running). Adapts to a new
        output device rate / block size; buffered data is discarded. */
    void setConsumerFormat (double consumerSampleRate, int consumerMaxBlockSize);

    bool isPrepared() const noexcept { return channels > 0; }
    int getNumChannels() const noexcept { return channels; }
    double getProducerSampleRate() const noexcept { return producerRate; }

    // ---- Producer thread -------------------------------------------------------
    /** Pushes interleaved frames. numSourceChannels may differ from the FIFO's
        channel count: mono is duplicated, surround -> stereo is downmixed
        (ITU-R BS.775), otherwise channels are copied and the rest zeroed.
        Returns the number of frames accepted. */
    int push (const float* interleaved, int numFrames, int numSourceChannels) noexcept;

    // ---- Consumer thread -------------------------------------------------------
    /** Renders exactly numFrames (<= consumerMaxBlockSize) planar frames into
        dest[0 .. numDestChannels-1]; adds to dest when addToDest is true,
        otherwise overwrites. Extra destination channels are left untouched when
        adding and zeroed otherwise. Returns false if the block was (partly)
        silence because the FIFO is priming or ran dry. */
    bool pull (float* const* dest, int numDestChannels, int numFrames, bool addToDest) noexcept;

    /** Consumer thread: discard buffered data and re-prime. */
    void restart() noexcept;

    Stats getStats() const noexcept;

private:
    static constexpr int kChunkFrames = 1024; // producer conversion chunk
    static constexpr double kMaxCorrection = 0.005;
    // Loop: de/dt = drift - c, c = Kp e + Ki int(e)  ->  omega_n = sqrt(Ki),
    // zeta = Kp / (2 sqrt(Ki)). Critically damped at 0.15 rad/s; the 1.5 s
    // fill smoothing (pole at 0.67 rad/s) suppresses the packet/block beat
    // pattern in the fill measurement.
    static constexpr double kKp = 0.3;        // 1/s
    static constexpr double kKi = 0.0225;     // 1/s^2
    static constexpr double kFillSmoothingSeconds = 1.5;

    /** Converts (and sanitises) into producerScratch; returns the number of samples muted. */
    int convertChunk (const float* src, int numFrames, int srcChannels) noexcept;
    double computeTargetFrames() const noexcept;
    void renderFadeToSilence (float* const* dest, int numDestChannels, int numFrames, bool addToDest) noexcept;
    void startStreaming (double fillFrames) noexcept;

    // Shared
    SpscRing<float> ring;
    int channels = 0;
    double producerRate = 48000.0;

    // Producer side
    std::vector<float> producerScratch;
    flub::LfeFold lfeFold; // the surround downmix's LFE path (docs/11 E01, at virt.lfe's default)
    std::atomic<float> burstEstimate { 0.0f };  // decaying peak of push sizes (frames)
    std::atomic<uint64_t> framesPushed { 0 }, ringFullDrops { 0 }, corruptSamples { 0 };

    // Consumer side (only touched by the consumer or while it is stopped)
    double consumerRate = 48000.0, nominalRatio = 1.0;
    int consumerBlock = 512, fadeLength = 240;
    std::vector<float> staging; // interleaved frames popped for one block
    // The last Kernel::kTaps frames of each channel, written twice (at i and
    // i + kTaps) so that [historyPos, historyPos + kTaps) is always the
    // window, oldest first.
    std::array<std::array<float, 2 * Kernel::kTaps>, kMaxChannels> history {};
    std::array<float, Kernel::kTaps> kernelScratch {}; // the coefficients of the current output frame
    const float* kernelRows = nullptr;                  // Kernel::table(), fetched by prepare()
    int historyPos = 0;
    std::array<float, kMaxChannels> lastOut {};
    uint64_t phase = 0;         // 32.32 fixed-point position inside the window
    double smoothedFill = 0.0, integral = 0.0;
    int fadeInRemaining = 0;
    bool streamingState = false;

    // Telemetry (consumer writes, anyone reads)
    std::atomic<uint64_t> underruns { 0 }, overflows { 0 }, overflowDrops { 0 };
    std::atomic<float> fillMsStat { 0.0f }, targetMsStat { 0.0f }, correctionPpmStat { 0.0f };
    std::atomic<bool> streamingStat { false };
};
} // namespace flub::app
