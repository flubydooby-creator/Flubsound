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
//                          cubic Hermite fractional resampler, ratio =
//                          nominal (producerRate / consumerRate) * (1 + c)
//                                                    |
//               PI controller: c = Kp * e + Ki * integral(e),
//               e = smoothed fill level - target (in seconds)
//
// * Target fill = 2 device blocks (in producer frames), raised automatically
//   to "largest recent producer burst + 1 device block" because capture APIs
//   deliver in packets (e.g. 10 ms) that can be larger than a device block.
// * The loop is critically damped with a natural frequency of ~0.3 rad/s and
//   the correction is clamped to +-0.5 %, so pitch deviations are inaudible;
//   typical drifts (< 500 ppm) are absorbed without any dropout.
// * Underrun (not enough data for a block): the block fades from the last
//   output value to silence (no DC step), is counted, and the FIFO re-primes
//   until the fill is back at target; playback then resumes with a 5 ms fade-in.
// * Overflow (fill above a high-water mark, e.g. after the device stalled):
//   the consumer drops the OLDEST frames down to the target and counts it. If
//   the ring is completely full the producer has no choice but to drop the
//   newest frames; both are reported in Stats::droppedFrames.
//
// Threading: exactly one producer thread and one consumer thread.
//   prepare()             non-RT, neither side running
//   setConsumerFormat()   non-RT, consumer (audio callback) not running; the
//                         producer may keep pushing
//   push()                producer thread, RT-safe (no locks, no allocation)
//   pull() / restart()    consumer thread, RT-safe
//   getStats()            any thread (relaxed atomics)
//
// Large nominal ratios (e.g. 96 kHz capture into a 48 kHz device) work but
// the Hermite interpolator does not band-limit; the host therefore requests
// captures at the device rate so the nominal ratio is 1.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/SpscRing.h"

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
        float fillMs = 0.0f;            // smoothed fill level
        float targetMs = 0.0f;          // current target fill level
        float correctionPpm = 0.0f;     // PI controller output (+ = consuming faster)
        bool streaming = false;         // false while priming / after an underrun
    };

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
    static constexpr double kKp = 0.54;       // 1/s   (zeta ~ 0.9)
    static constexpr double kKi = 0.09;       // 1/s^2 (omega_n ~ 0.3 rad/s)
    static constexpr double kFillSmoothingSeconds = 0.5;

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
    std::atomic<float> burstEstimate { 0.0f };  // decaying peak of push sizes (frames)
    std::atomic<uint64_t> framesPushed { 0 }, ringFullDrops { 0 };

    // Consumer side (only touched by the consumer or while it is stopped)
    double consumerRate = 48000.0, nominalRatio = 1.0;
    int consumerBlock = 512, fadeLength = 240;
    std::vector<float> staging; // interleaved frames popped for one block
    std::array<std::array<float, 4>, kMaxChannels> history {};
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
