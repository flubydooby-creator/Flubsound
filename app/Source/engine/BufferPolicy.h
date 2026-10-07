// Flubsound Pro - the device buffer size per latency profile (docs/11 E42c).
//
// Until docs/11 E42c the app never asked for a buffer size: JUCE opened every
// device at its default, which for WASAPI's "Windows Audio (Low Latency
// Mode)" (IAudioClient3) is the engine's DEFAULT period, 480 frames = 10 ms
// at 48 kHz, even on a device whose driver allows 128 (2.7 ms). Now, while
// "Automatic buffer size" is on (Settings > Audio, the default), the host
// asks for:
//
//   Quality     : the device's default (what the app always used)
//   Balanced    : the available size nearest to 5 ms (ties: the larger)
//   Low Latency : the smallest available size of at least 1.33 ms (64
//                 samples at 48 kHz, docs/11 E42c's ">= 64")
//
// never more than the device's default, and never less than the device's
// back-off floor (below). Sizes are what the device offers
// (juce::AudioIODevice::getAvailableBufferSizes): with IAudioClient3 the
// minimum period plus multiples of its fundamental period up to the
// maximum; a driver without small periods (USB Audio class drivers, many
// wireless dongles: minimum = default = maximum = 480) offers one size and
// nothing changes. A buffer the user picks in Settings > Audio always wins
// (Automatic turns itself off); the latency profile the automatic overload
// response steps to never shrinks the buffer (EngineController passes the
// profile the user chose).
//
// Back-off (the safety net): Backoff watches the overload watchdog's glitch
// count (xruns, overrunning and late callbacks; OverloadWatchdog) at its 2 Hz
// poll. More than 2 glitches within 10 s, or a sustained overload starting,
// raises the buffer one available size (AudioEngineHost::raiseBufferOneStep),
// at most up to the device's default, and that size becomes the device's
// floor (persisted per device type and output: the next start does not
// glitch at the smaller size again). After a step it waits 10 s (the device
// restart's own glitch does not count). Turning Automatic off and on again
// forgets the floor.
//
// Plain C++, no JUCE, no threads, no clock: decision logic only (like
// AutoLoadReducer); AudioEngineHost applies it.
#pragma once

#include "flub/engine/Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace flub::app::buffer
{
using Profile = flub::param::LatencyProfileValue;

/** Low Latency asks for at least this much (64 samples at 48 kHz). */
inline constexpr double kLowLatencyMinMs = 64.0 / 48.0;
/** Balanced asks for the size nearest to this. */
inline constexpr double kBalancedTargetMs = 5.0;

/** Why the target is what it is (for the Settings line). */
enum class Reason : uint8_t
{
    DeviceDefault, // Quality, or the profile's choice would have been above the default
    Smallest,      // Low Latency: the smallest size of at least kLowLatencyMinMs
    NearFiveMs,    // Balanced: the size nearest to kBalancedTargetMs
    Floor,         // raised to the device's back-off floor after glitches
    OnlyOne        // the device offers one size: nothing to choose
};

/** Sorted, unique, positive. */
inline std::vector<int> normalise (std::vector<int> sizes)
{
    sizes.erase (std::remove_if (sizes.begin(), sizes.end(), [] (int s) { return s <= 0; }), sizes.end());
    std::sort (sizes.begin(), sizes.end());
    sizes.erase (std::unique (sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

/** The smallest of `sizes` (normalised) that is at least n; the largest when none is. */
inline int smallestAtLeast (const std::vector<int>& sizes, int n)
{
    for (const int s : sizes)
        if (s >= n)
            return s;
    return sizes.empty() ? n : sizes.back();
}

/** The size of `sizes` (normalised) nearest to n; on a tie the larger. */
inline int nearest (const std::vector<int>& sizes, int n)
{
    int best = sizes.empty() ? n : sizes.front();
    for (const int s : sizes)
        if (std::abs (s - n) <= std::abs (best - n))
            best = s;
    return best;
}

struct Choice
{
    int samples = 0;
    Reason reason = Reason::DeviceDefault;
};

/** The size to ask for (see the header). `available`: the device's sizes in
    any order; `defaultSize`: its default (<= 0: the largest); `floorSamples`:
    its back-off floor (0 = none). */
inline Choice choose (Profile profile, std::vector<int> available, int defaultSize, double sampleRate, int floorSamples = 0)
{
    const auto sizes = normalise (std::move (available));
    if (sizes.empty())
        return { std::max (0, defaultSize), Reason::DeviceDefault };
    // JUCE opens the size nearest to the one asked for: so does the default.
    const int deflt = nearest (sizes, defaultSize > 0 ? defaultSize : sizes.back());
    if (sizes.size() == 1)
        return { sizes.front(), Reason::OnlyOne };

    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    Choice c;
    switch (profile)
    {
        case Profile::LowLatency:
            c = { smallestAtLeast (sizes, static_cast<int> (std::ceil (kLowLatencyMinMs * 0.001 * rate - 1.0e-9))), Reason::Smallest };
            break;
        case Profile::Balanced:
            c = { nearest (sizes, static_cast<int> (std::lround (kBalancedTargetMs * 0.001 * rate))), Reason::NearFiveMs };
            break;
        case Profile::Quality:
            c = { deflt, Reason::DeviceDefault };
            break;
    }
    if (c.samples > deflt)
        c = { deflt, Reason::DeviceDefault }; // never above what the app used before
    if (floorSamples > c.samples)
        c = { std::min (smallestAtLeast (sizes, floorSamples), deflt), Reason::Floor };
    return c;
}

/** The back-off's next size: the smallest available size above `current`,
    at most the device's default; nothing at the top. */
inline std::optional<int> nextLarger (std::vector<int> available, int current, int defaultSize)
{
    const auto sizes = normalise (std::move (available));
    if (sizes.empty())
        return std::nullopt;
    const int deflt = nearest (sizes, defaultSize > 0 ? defaultSize : sizes.back());
    for (const int s : sizes)
        if (s > current)
            return s <= deflt ? std::optional<int> (s) : std::nullopt;
    return std::nullopt;
}

/** When to raise the buffer (see the header). One update() per watchdog
    poll (2 Hz). */
class Backoff
{
public:
    struct Config
    {
        int windowPolls = 20;  // 10 s at 2 Hz ...
        int maxGlitches = 2;   // ... with more glitches than this: a step
        int holdPolls = 20;    // after a step (or a device start), this long before the next
    };

    Backoff() = default;
    explicit Backoff (const Config& c) : config (sanitise (c)) {}

    const Config& getConfig() const noexcept { return config; }

    /** `running`: a device runs; `sessionGlitches`: the watchdog's cumulative
        glitch count (OverloadWatchdog::State::glitches); `overloadStarted`:
        this poll started a sustained overload. True: raise the buffer one
        size now (the caller does; it is then held for holdPolls). */
    bool update (bool running, uint64_t sessionGlitches, bool overloadStarted) noexcept
    {
        const uint64_t fresh = haveBaseline && sessionGlitches >= lastGlitches ? sessionGlitches - lastGlitches : 0;
        lastGlitches = sessionGlitches;
        haveBaseline = true;
        if (! running)
        {
            clearWindow();
            hold = config.holdPolls; // a device (re)start glitches on its own
            return false;
        }
        if (hold > 0)
        {
            --hold;
            push (0);
            return false;
        }
        push (fresh);
        if (windowSum > static_cast<uint64_t> (config.maxGlitches) || overloadStarted)
        {
            clearWindow();
            hold = config.holdPolls;
            ++steps;
            return true;
        }
        return false;
    }

    /** The buffer was just changed by something else (the profile, the user):
        forget the window and hold for holdPolls, since the device restart
        glitches on its own. */
    void restarted() noexcept
    {
        clearWindow();
        hold = config.holdPolls;
    }

    uint64_t getSteps() const noexcept { return steps; }
    uint64_t getWindowGlitches() const noexcept { return windowSum; }

private:
    static constexpr int kMaxWindow = 64;

    static Config sanitise (Config c) noexcept
    {
        c.windowPolls = std::clamp (c.windowPolls, 1, kMaxWindow);
        c.maxGlitches = std::max (0, c.maxGlitches);
        c.holdPolls = std::max (0, c.holdPolls);
        return c;
    }

    void push (uint64_t fresh) noexcept
    {
        windowSum -= window[pos];
        window[pos] = fresh;
        windowSum += fresh;
        pos = (pos + 1) % static_cast<size_t> (config.windowPolls);
    }

    void clearWindow() noexcept
    {
        window.fill (0);
        windowSum = 0;
        pos = 0;
    }

    Config config;
    std::array<uint64_t, kMaxWindow> window {};
    uint64_t windowSum = 0, lastGlitches = 0, steps = 0;
    size_t pos = 0;
    int hold = 0;
    bool haveBaseline = false;
};
} // namespace flub::app::buffer
