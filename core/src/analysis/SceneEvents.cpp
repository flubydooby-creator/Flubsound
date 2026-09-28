// Flubsound Pro - programme background and scene events (see the header).
#include "flub/analysis/SceneEvents.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace flub
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// Loud frames closer together than this form one event (a burst of fire).
constexpr double kLoudMergeMs = 150.0;

// Frame powers are floored here before they are averaged or turned into dB,
// so digital silence reads as a finite, very low level.
constexpr double kPowerFloor = 1.0e-16; // -160 dB

float toDb (double power) noexcept
{
    return static_cast<float> (10.0 * std::log10 (std::max (power, kPowerFloor)));
}

/** RBJ band-pass (0 dB peak), in double, in place. */
void bandPass (std::vector<double>& x, double f0, double q, double fs) noexcept
{
    const double w0 = 2.0 * kPi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
    const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (double& v : x)
    {
        const double in = v;
        const double out = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;
        v = out;
    }
}

int framesOf (double ms, double frameMs) noexcept
{
    return std::max (1, static_cast<int> (std::lround (ms / frameMs)));
}

/** Median of frame levels [from, to) (dB), using `scratch`. */
float medianDb (const std::vector<SceneFrame>& frames, int from, int to, std::vector<float>& scratch)
{
    scratch.clear();
    for (int i = from; i < to; ++i)
        scratch.push_back (frames[static_cast<size_t> (i)].levelDb);
    const auto mid = scratch.begin() + static_cast<std::ptrdiff_t> (scratch.size() / 2);
    std::nth_element (scratch.begin(), mid, scratch.end());
    return *mid;
}
} // namespace

const char* sceneEventName (SceneEventType type) noexcept
{
    switch (type)
    {
        case SceneEventType::Onset: return "onset";
        case SceneEventType::Loud: return "loud";
        case SceneEventType::Silence: return "silence";
        case SceneEventType::LevelChange: return "level-change";
    }
    return "";
}

SceneAnalysis analyseSceneEvents (const std::vector<std::vector<float>>& channels, double sampleRate, const SceneEventSettings& s)
{
    SceneAnalysis a;
    if (channels.empty() || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate))
        return a;
    const double frameMs = std::clamp (std::isfinite (s.frameMs) ? s.frameMs : 10.0, 1.0, 1000.0);
    const int frameLength = std::max (1, static_cast<int> (std::lround (frameMs * 0.001 * sampleRate)));
    a.frameSeconds = frameLength / sampleRate;
    size_t length = channels[0].size();
    for (const auto& ch : channels)
        length = std::min (length, ch.size());
    const int numFrames = static_cast<int> (length / static_cast<size_t> (frameLength));

    // ---- per-frame level and peak (mean square over all channels) ---------
    std::vector<double> power (static_cast<size_t> (numFrames), 0.0), peak (static_cast<size_t> (numFrames), 0.0);
    const bool band = s.bandHz > 0.0 && s.bandHz < 0.5 * sampleRate && s.bandQ > 0.0;
    std::vector<double> x;
    for (const auto& ch : channels)
    {
        x.assign (ch.begin(), ch.begin() + static_cast<std::ptrdiff_t> (length));
        for (double& v : x)
            if (! std::isfinite (v))
                v = 0.0;
        if (band)
            bandPass (x, s.bandHz, s.bandQ, sampleRate);
        for (int f = 0; f < numFrames; ++f)
        {
            const size_t begin = static_cast<size_t> (f) * static_cast<size_t> (frameLength);
            double acc = 0.0, pk = 0.0;
            for (size_t i = begin; i < begin + static_cast<size_t> (frameLength); ++i)
            {
                acc += x[i] * x[i];
                pk = std::max (pk, std::abs (x[i]));
            }
            power[static_cast<size_t> (f)] += acc;
            peak[static_cast<size_t> (f)] = std::max (peak[static_cast<size_t> (f)], pk);
        }
    }
    const double perFrame = static_cast<double> (frameLength) * static_cast<double> (channels.size());
    BackgroundTracker tracker;
    tracker.prepare (1.0 / a.frameSeconds);
    a.frames.resize (static_cast<size_t> (numFrames));
    for (int f = 0; f < numFrames; ++f)
    {
        auto& fr = a.frames[static_cast<size_t> (f)];
        fr.levelDb = toDb (power[static_cast<size_t> (f)] / perFrame);
        fr.peakDb = toDb (peak[static_cast<size_t> (f)] * peak[static_cast<size_t> (f)]);
        fr.backgroundDb = tracker.update (fr.levelDb, s.floorDb);
    }

    // ---- runs of frames: onsets, loud events, silences --------------------
    const auto time = [&] (int f) { return f * a.frameSeconds; };
    const auto isLoud = [&] (const SceneFrame& fr) { return fr.peakDb >= fr.backgroundDb + s.loudOverBackgroundDb || fr.peakDb >= s.loudAbsoluteDb; };
    const int maxOnset = framesOf (s.maxOnsetMs, frameMs), minSilence = framesOf (s.minSilenceMs, frameMs);
    const int loudGap = framesOf (kLoudMergeMs, frameMs);
    for (int f = 0; f < numFrames;)
    {
        const auto& fr = a.frames[static_cast<size_t> (f)];
        if (isLoud (fr))
        {
            // One Loud event per burst: loud frames with gaps under kLoudMergeMs.
            SceneEvent e { SceneEventType::Loud, time (f), 0.0, fr.peakDb, fr.peakDb - fr.backgroundDb };
            int last = f, g = f + 1;
            for (; g < numFrames && g - last <= loudGap; ++g)
            {
                const auto& fg = a.frames[static_cast<size_t> (g)];
                if (isLoud (fg))
                {
                    last = g;
                    e.levelDb = std::max (e.levelDb, fg.peakDb);
                    e.overBackgroundDb = std::max (e.overBackgroundDb, fg.peakDb - fg.backgroundDb);
                }
            }
            e.endSeconds = time (last + 1);
            a.events.push_back (e);
            f = last + 1;
        }
        else if (fr.levelDb >= fr.backgroundDb + s.onsetDb)
        {
            SceneEvent e { SceneEventType::Onset, time (f), 0.0, fr.levelDb, fr.levelDb - fr.backgroundDb };
            int g = f + 1;
            for (; g < numFrames; ++g)
            {
                const auto& fg = a.frames[static_cast<size_t> (g)];
                if (fg.levelDb < fg.backgroundDb + s.onsetDb || isLoud (fg))
                    break;
                e.levelDb = std::max (e.levelDb, fg.levelDb);
                e.overBackgroundDb = std::max (e.overBackgroundDb, fg.levelDb - fg.backgroundDb);
            }
            e.endSeconds = time (g);
            if (g - f <= maxOnset)
                a.events.push_back (e); // longer: a sustained sound, not a cue
            f = g;
        }
        else if (fr.levelDb < s.silenceDb)
        {
            int g = f + 1;
            while (g < numFrames && a.frames[static_cast<size_t> (g)].levelDb < s.silenceDb)
                ++g;
            if (g - f >= minSilence)
            {
                float top = kMinusInfDb;
                for (int i = f; i < g; ++i)
                    top = std::max (top, a.frames[static_cast<size_t> (i)].levelDb);
                a.events.push_back ({ SceneEventType::Silence, time (f), time (g), top, 0.0f });
            }
            f = g;
        }
        else
            ++f;
    }

    // ---- level changes: median level after vs before each frame -----------
    // Medians, so a burst of fire or the pauses in speech do not read as a
    // new level; one event per run of frames past changeDb (same sign), at
    // its largest step.
    const int window = framesOf (s.changeWindowMs, frameMs);
    std::vector<float> scratch;
    scratch.reserve (static_cast<size_t> (window));
    SceneEvent best {};
    bool inRun = false;
    for (int f = window; f + window <= numFrames; ++f)
    {
        const float before = medianDb (a.frames, f - window, f, scratch);
        const float after = medianDb (a.frames, f, f + window, scratch);
        const float d = after - before;
        const bool past = std::abs (d) >= s.changeDb;
        if (past && inRun && (d > 0.0f) == (best.overBackgroundDb > 0.0f))
        {
            if (std::abs (d) > std::abs (best.overBackgroundDb))
                best = { SceneEventType::LevelChange, time (f), time (f), after, d };
            continue;
        }
        if (inRun)
            a.events.push_back (best);
        inRun = past;
        if (past)
            best = { SceneEventType::LevelChange, time (f), time (f), after, d };
    }
    if (inRun)
        a.events.push_back (best);

    std::stable_sort (a.events.begin(), a.events.end(), [] (const SceneEvent& l, const SceneEvent& r) { return l.startSeconds < r.startSeconds; });
    return a;
}
} // namespace flub
