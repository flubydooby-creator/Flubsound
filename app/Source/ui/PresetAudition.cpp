#include "PresetAudition.h"

#include "engine/TestSignalGenerator.h"

#include "OfflineRenderer.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr int kBlockSize = 512;
constexpr float kSilentLufs = -70.0f;

/** Integrated loudness of planar stereo `channels` from sample `from` on. */
float integratedLufs (const std::vector<std::vector<float>>& channels, double sampleRate, int from)
{
    if (channels.size() < 2 || static_cast<int> (channels[0].size()) <= from)
        return -160.0f;
    flub::LoudnessMeter meter;
    meter.prepare (sampleRate, 2);
    const int n = static_cast<int> (channels[0].size());
    for (int pos = from; pos < n; pos += kBlockSize)
    {
        const std::array<float*, 2> ptrs { const_cast<float*> (channels[0].data()), const_cast<float*> (channels[1].data()) };
        meter.process (flub::AudioBlock (ptrs.data(), 2, std::min (kBlockSize, n - pos), pos));
    }
    return meter.getIntegratedLufs();
}
} // namespace

// =============================================================================
// PresetLoudnessEstimator
// =============================================================================
PresetLoudnessEstimator::PresetLoudnessEstimator (double rate)
    : juce::Thread ("Flubsound preset loudness"), sampleRate (rate > 0.0 ? rate : 48000.0)
{
}

PresetLoudnessEstimator::~PresetLoudnessEstimator()
{
    cancelPendingUpdate();
    abortRender.store (true);
    signalThreadShouldExit();
    notify();
    stopThread (4000);
}

flub::io::AudioFileData PresetLoudnessEstimator::makeReferenceProgramme (double rate)
{
    flub::io::AudioFileData data;
    data.sampleRate = rate;
    data.numChannels = 2;
    const auto n = static_cast<size_t> (kProgrammeSeconds * rate);
    data.channels.assign (2, std::vector<float> (n, 0.0f));

    TestSignalGenerator generator (rate);
    generator.setProgramme (0, TestSignalGenerator::Programme::Music, 0.0f);
    const std::array<float*, 2> ptrs { data.channels[0].data(), data.channels[1].data() };
    for (size_t pos = 0; pos < n; pos += kBlockSize)
        generator.renderStrip (0, flub::AudioBlock (ptrs.data(), 2, static_cast<int> (std::min<size_t> (kBlockSize, n - pos)), static_cast<int> (pos)));
    return data;
}

std::optional<float> PresetLoudnessEstimator::estimateGainLu (const flub::io::AudioFileData& programme, const std::vector<float>& values,
                                                              const std::atomic<bool>* abort)
{
    if (values.size() != static_cast<size_t> (kNumParams) || programme.numChannels != 2 || programme.sampleRate <= 0.0)
        return {};

    auto processed = values;
    processed[static_cast<size_t> (BypassAll)] = 0.0f; // the processed sound, whatever the master Bypass is doing
    std::vector<std::vector<float>> out;
    int latency = 0;
    std::string error;
    if (! flub::cli::renderPass (programme, processed, kBlockSize, out, latency, error, abort))
        return {};

    const int settle = static_cast<int> (kSettleSeconds * programme.sampleRate);
    const float in = integratedLufs (programme.channels, programme.sampleRate, settle);
    const float o = integratedLufs (out, programme.sampleRate, settle);
    if (! (in > kSilentLufs && o > kSilentLufs))
        return {};
    return o - in;
}

int PresetLoudnessEstimator::levelBucket (float lufs) noexcept
{
    return juce::roundToInt (std::clamp (std::isfinite (lufs) ? lufs : kDefaultLevelLufs, kMinLevelLufs, kMaxLevelLufs));
}

uint64_t PresetLoudnessEstimator::keyOf (const std::vector<float>& values, float levelLufs) noexcept
{
    uint64_t h = 1469598103934665603ull;
    const auto level = static_cast<uint32_t> (levelBucket (levelLufs) + 1000);
    for (int b = 0; b < 4; ++b)
    {
        h ^= (level >> (8 * b)) & 0xffu;
        h *= 1099511628211ull;
    }
    for (const float v : values)
    {
        uint32_t bits = 0;
        std::memcpy (&bits, &v, sizeof (bits));
        for (int b = 0; b < 4; ++b)
        {
            h ^= (bits >> (8 * b)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::optional<float> PresetLoudnessEstimator::find (const std::vector<float>& values, float levelLufs) const
{
    const auto key = keyOf (values, levelLufs);
    const juce::ScopedLock sl (lock);
    const auto it = results.find (key);
    if (it == results.end())
        return {};
    return it->second;
}

void PresetLoudnessEstimator::request (const std::vector<float>& values, float levelLufs, bool urgent)
{
    if (values.size() != static_cast<size_t> (kNumParams))
        return;
    const auto key = keyOf (values, levelLufs);
    {
        const juce::ScopedLock sl (lock);
        if (results.count (key) > 0 || (jobRunning && runningKey == key))
            return;
        const auto it = std::find_if (queue.begin(), queue.end(), [key] (const Job& j) { return j.key == key; });
        if (it != queue.end())
        {
            if (urgent && it != queue.begin())
            {
                auto job = std::move (*it);
                queue.erase (it);
                queue.push_front (std::move (job));
            }
            return;
        }
        if (urgent)
            queue.push_front ({ key, values, levelBucket (levelLufs) });
        else
            queue.push_back ({ key, values, levelBucket (levelLufs) });
    }
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
    notify();
}

int PresetLoudnessEstimator::getQueuedCount() const
{
    const juce::ScopedLock sl (lock);
    return static_cast<int> (queue.size()) + (jobRunning ? 1 : 0);
}

void PresetLoudnessEstimator::run()
{
    const auto reference = makeReferenceProgramme (sampleRate);
    const float referenceLufs = integratedLufs (reference.channels, sampleRate, static_cast<int> (kSettleSeconds * sampleRate));
    auto programme = reference;
    int programmeLevel = std::numeric_limits<int>::min();
    while (! threadShouldExit())
    {
        Job job;
        bool have = false;
        {
            const juce::ScopedLock sl (lock);
            if (! queue.empty())
            {
                job = std::move (queue.front());
                queue.pop_front();
                runningKey = job.key;
                jobRunning = have = true;
            }
        }
        if (! have)
        {
            wait (1000);
            continue;
        }

        if (job.level != programmeLevel)
        {
            // The reference, scaled to the level asked for.
            const auto gain = flub::dbToGain (static_cast<float> (job.level) - referenceLufs);
            for (size_t c = 0; c < reference.channels.size(); ++c)
                for (size_t i = 0; i < reference.channels[c].size(); ++i)
                    programme.channels[c][i] = reference.channels[c][i] * gain;
            programmeLevel = job.level;
        }
        const auto estimate = estimateGainLu (programme, job.values, &abortRender);
        if (threadShouldExit())
            break;
        {
            const juce::ScopedLock sl (lock);
            results[job.key] = estimate.value_or (std::numeric_limits<float>::quiet_NaN());
            jobRunning = false;
        }
        triggerAsyncUpdate();
    }
}

void PresetLoudnessEstimator::handleAsyncUpdate()
{
    if (onEstimate != nullptr)
        onEstimate();
}

// =============================================================================
// PresetAudition
// =============================================================================
PresetAudition::Trims PresetAudition::matchTrims (float originalGainLu, float previewGainLu) noexcept
{
    if (! (std::isfinite (originalGainLu) && std::isfinite (previewGainLu)))
        return {};
    const float quieter = std::min (originalGainLu, previewGainLu);
    return { std::max (-kMaxTrimDb, quieter - previewGainLu), std::max (-kMaxTrimDb, quieter - originalGainLu) };
}

PresetAudition::PresetAudition (EngineController& c)
    : controller (c)
{
}

PresetAudition::~PresetAudition() { cancel(); }

void PresetAudition::begin (int s)
{
    cancel();
    if (s < 0 || s >= controller.getNumStrips())
        return;
    strip = s;
    auto& store = controller.getParams (strip);
    bank = store.getActiveBank();
    original.resize (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        original[static_cast<size_t> (i)] = store.get (bank, i);
    written = original;
    previewId = {};
    presetIdAtBegin = controller.getCurrentPresetId (strip);
    baseGainDb = appliedGainDb = controller.getHost().getStripGainDb (strip);
    trimDb = 0.0f;
}

std::vector<float> PresetAudition::valuesFor (const PresetInfo& preset, juce::StringArray* warnings) const
{
    flub::preset::Preset p;
    juce::String error;
    if (! controller.getPresetManager().readPreset (preset, p, error) || p.values.size() != static_cast<size_t> (kNumParams))
        return {};
    if (warnings != nullptr)
        for (const auto& w : p.warnings)
            warnings->add (juce::String::fromUTF8 (w.c_str()));

    // The bank's app state stays (preset::applyPresetToStore never writes it).
    auto values = original;
    if (values.empty())
    {
        const int s = controller.getSelectedStrip();
        const auto& store = controller.getParams (s);
        values.resize (static_cast<size_t> (kNumParams));
        for (int i = 0; i < kNumParams; ++i)
            values[static_cast<size_t> (i)] = store.get (store.getActiveBank(), i);
    }
    for (int i = 0; i < kNumParams; ++i)
        if (! flub::preset::isAppState (i))
            values[static_cast<size_t> (i)] = p.values[static_cast<size_t> (i)];
    return values;
}

bool PresetAudition::preview (const PresetInfo& preset, juce::String& error, juce::StringArray* warnings)
{
    if (! isActive())
        begin (controller.getSelectedStrip());
    if (! isActive())
    {
        error = "No strip to preview on";
        return false;
    }
    const auto values = valuesFor (preset, warnings);
    if (values.empty())
    {
        error = "Could not read the preset \"" + preset.name + "\"";
        return false;
    }
    write (values);
    previewId = preset.id;
    return true;
}

void PresetAudition::previewOriginal()
{
    if (! isActive())
        return;
    write (original);
    previewId = {};
}

void PresetAudition::write (const std::vector<float>& values)
{
    if (strip >= controller.getNumStrips())
        return;
    auto& store = controller.getParams (strip);
    for (int i = 0; i < kNumParams; ++i)
    {
        if (flub::preset::isAppState (i))
            continue;
        const auto k = static_cast<size_t> (i);
        if (store.get (bank, i) != values[k])
            store.set (bank, i, values[k]);
        written[k] = store.get (bank, i); // as stored (clamped), so cancel() recognises it
    }
    // The strip-state autosave keeps saving the sound before the preview
    // (docs/11 E40): a crash now must not make the preview the saved state.
    controller.setPreviewInProgress (strip, bank, original, written);
}

void PresetAudition::setTrimDb (float db)
{
    if (! isActive() || ! std::isfinite (db))
        return;
    auto& host = controller.getHost();
    const float current = host.getStripGainDb (strip);
    if (current != appliedGainDb)
        baseGainDb = current; // the strip gain moved (user gain, ChatMix): the trim follows it
    trimDb = std::clamp (db, -kMaxTrimDb, 0.0f);
    host.setStripGainDb (strip, baseGainDb + trimDb);
    appliedGainDb = host.getStripGainDb (strip);
}

void PresetAudition::releaseTrim()
{
    if (! isActive())
        return;
    auto& host = controller.getHost();
    if (host.getStripGainDb (strip) == appliedGainDb)
        host.setStripGainDb (strip, baseGainDb);
    trimDb = 0.0f;
}

void PresetAudition::cancel()
{
    if (! isActive())
        return;
    releaseTrim();
    if (strip < controller.getNumStrips())
    {
        auto& store = controller.getParams (strip);
        for (int i = 0; i < kNumParams; ++i)
        {
            const auto k = static_cast<size_t> (i);
            if (! flub::preset::isAppState (i) && store.get (bank, i) == written[k] && written[k] != original[k])
                store.set (bank, i, original[k]);
        }
    }
    controller.clearPreviewInProgress();
    strip = -1;
    previewId = {};
}

bool PresetAudition::commit (const PresetInfo& preset, juce::String& error)
{
    const int s = isActive() ? strip : controller.getSelectedStrip();
    abandon();
    return controller.loadPreset (preset, s, error);
}

void PresetAudition::abandon()
{
    releaseTrim();
    if (isActive())
        controller.clearPreviewInProgress();
    strip = -1;
    previewId = {};
}
} // namespace flub::app::ui
