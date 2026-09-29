#include "PresetAudition.h"

#include "engine/TestSignalGenerator.h"

#include "OfflineRenderer.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr int kBlockSize = 512;
constexpr float kSilentLufs = -70.0f;

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& info : layout())
        v.push_back (info.defaultValue);
    return v;
}

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

flub::io::AudioFileData PresetLoudnessEstimator::makeReferenceProgramme (double rate, int channels)
{
    const bool surround = channels == 8;
    flub::io::AudioFileData data;
    data.sampleRate = rate;
    data.numChannels = surround ? 8 : 2;
    const auto n = static_cast<size_t> (kProgrammeSeconds * rate);
    data.channels.assign (static_cast<size_t> (data.numChannels), std::vector<float> (n, 0.0f));

    TestSignalGenerator generator (rate);
    generator.setProgramme (0, surround ? TestSignalGenerator::Programme::Game71 : TestSignalGenerator::Programme::Music, 0.0f);
    std::array<float*, 8> ptrs {};
    for (size_t c = 0; c < data.channels.size(); ++c)
        ptrs[c] = data.channels[c].data();
    for (size_t pos = 0; pos < n; pos += kBlockSize)
        generator.renderStrip (0, flub::AudioBlock (ptrs.data(), data.numChannels, static_cast<int> (std::min<size_t> (kBlockSize, n - pos)),
                                                    static_cast<int> (pos)));
    return data;
}

namespace
{
/** renderPass with one module audition-bypassed (the module card's ear):
    the same private store / chain / prime as the CLI's pass, without its
    latency alignment (the estimate is an integrated loudness). */
bool renderListenPass (const flub::io::AudioFileData& input, const std::vector<float>& values, int listenBypassId,
                       std::vector<std::vector<float>>& out, const std::atomic<bool>* abort)
{
    const int channels = input.numChannels;
    ParameterStore store;
    for (int id = 0; id < kNumParams; ++id)
        store.set (Bank::A, id, values[static_cast<size_t> (id)]);
    store.setActiveBank (Bank::A);
    flub::ProcessingChain chain (store);
    chain.prepare ({ input.sampleRate, kBlockSize, channels });
    chain.setAuditionBypass (listenBypassId, true);
    flub::AudioBuffer io (channels, kBlockSize);
    io.clear();
    chain.process (io.block (channels, kBlockSize));
    chain.reset();

    const auto n = input.numFrames();
    out.assign (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    for (int64_t pos = 0; pos < n; pos += kBlockSize)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed))
            return false;
        const int count = static_cast<int> (std::min<int64_t> (kBlockSize, n - pos));
        for (int c = 0; c < channels; ++c)
            std::memcpy (io.channel (c), input.channels[static_cast<size_t> (c)].data() + pos, sizeof (float) * static_cast<size_t> (count));
        chain.process (io.block (channels, count));
        for (int c = 0; c < 2; ++c)
            std::memcpy (out[static_cast<size_t> (c)].data() + pos, io.channel (c), sizeof (float) * static_cast<size_t> (count));
    }
    return true;
}
} // namespace

std::optional<float> PresetLoudnessEstimator::estimateGainLu (const flub::io::AudioFileData& programme, const std::vector<float>& values,
                                                              const std::atomic<bool>* abort, int listenBypassId)
{
    if (values.size() != static_cast<size_t> (kNumParams) || (programme.numChannels != 2 && programme.numChannels != 8)
        || programme.sampleRate <= 0.0)
        return {};

    auto processed = values;
    processed[static_cast<size_t> (BypassAll)] = 0.0f; // the processed sound, whatever the master Bypass is doing
    std::vector<std::vector<float>> out;
    if (listenBypassId >= 0)
    {
        if (! renderListenPass (programme, processed, listenBypassId, out, abort))
            return {};
    }
    else
    {
        int latency = 0;
        std::string error;
        if (! flub::cli::renderPass (programme, processed, kBlockSize, out, latency, error, abort))
            return {};
    }

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

uint64_t PresetLoudnessEstimator::keyOf (const std::vector<float>& values, float levelLufs, Variant variant) noexcept
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h] (uint32_t word)
    {
        for (int b = 0; b < 4; ++b)
        {
            h ^= (word >> (8 * b)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    mix (static_cast<uint32_t> (levelBucket (levelLufs) + 1000));
    // The default variant adds nothing, so stereo keys are the ones they always were.
    if (! (variant == Variant {}))
    {
        mix (static_cast<uint32_t> (variant.channels));
        mix (static_cast<uint32_t> (variant.listenBypassId + 1));
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

std::optional<float> PresetLoudnessEstimator::find (const std::vector<float>& values, float levelLufs, Variant variant) const
{
    const auto key = keyOf (values, levelLufs, variant);
    const juce::ScopedLock sl (lock);
    const auto it = results.find (key);
    if (it == results.end())
        return {};
    return it->second;
}

void PresetLoudnessEstimator::request (const std::vector<float>& values, float levelLufs, bool urgent, Variant variant)
{
    if (values.size() != static_cast<size_t> (kNumParams))
        return;
    variant.channels = variant.channels == 8 ? 8 : 2; // the two programmes there are
    const auto key = keyOf (values, levelLufs, variant);
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
            queue.push_front ({ key, values, levelBucket (levelLufs), variant });
        else
            queue.push_back ({ key, values, levelBucket (levelLufs), variant });
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
    // One reference per programme (stereo music, 7.1 game scene), made on
    // first use, and its copy scaled to the level of the current job.
    struct Programme
    {
        flub::io::AudioFileData reference, scaled;
        float referenceLufs = 0.0f;
        int level = std::numeric_limits<int>::min();
    };
    std::array<std::unique_ptr<Programme>, 2> programmes; // [0] stereo, [1] 7.1
    const auto programmeFor = [this, &programmes] (int channels) -> Programme&
    {
        auto& p = programmes[channels == 8 ? 1 : 0];
        if (p == nullptr)
        {
            p = std::make_unique<Programme>();
            p->reference = makeReferenceProgramme (sampleRate, channels);
            const int settle = static_cast<int> (kSettleSeconds * sampleRate);
            if (channels == 8)
            {
                // The level the strip's input meter reads: the chain's
                // stereo fold, i.e. its bypassed output (unmatched).
                auto values = defaultValues();
                values[static_cast<size_t> (BypassAll)] = 1.0f;
                values[static_cast<size_t> (LoudnessMatchBypass)] = 0.0f;
                std::vector<std::vector<float>> folded;
                int latency = 0;
                std::string error;
                p->referenceLufs = flub::cli::renderPass (p->reference, values, kBlockSize, folded, latency, error)
                                       ? integratedLufs (folded, sampleRate, settle)
                                       : integratedLufs (p->reference.channels, sampleRate, settle);
            }
            else
            {
                p->referenceLufs = integratedLufs (p->reference.channels, sampleRate, settle);
            }
            p->scaled = p->reference;
        }
        return *p;
    };
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

        auto& programme = programmeFor (job.variant.channels);
        if (job.level != programme.level)
        {
            // The reference, scaled to the level asked for.
            const auto gain = flub::dbToGain (static_cast<float> (job.level) - programme.referenceLufs);
            for (size_t c = 0; c < programme.reference.channels.size(); ++c)
                for (size_t i = 0; i < programme.reference.channels[c].size(); ++i)
                    programme.scaled.channels[c][i] = programme.reference.channels[c][i] * gain;
            programme.level = job.level;
        }
        const auto estimate = estimateGainLu (programme.scaled, job.values, &abortRender, job.variant.listenBypassId);
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

int PresetLoudnessEstimator::addListener (std::function<void()> listener)
{
    const int token = nextListenerToken++;
    listeners.emplace_back (token, std::move (listener));
    return token;
}

void PresetLoudnessEstimator::removeListener (int token)
{
    listeners.erase (std::remove_if (listeners.begin(), listeners.end(), [token] (const auto& l) { return l.first == token; }), listeners.end());
}

void PresetLoudnessEstimator::handleAsyncUpdate()
{
    // A copy: a listener may add or remove listeners (one removed meanwhile is skipped).
    const auto current = listeners;
    for (const auto& l : current)
    {
        const bool registered = std::any_of (listeners.begin(), listeners.end(), [&l] (const auto& r) { return r.first == l.first; });
        if (registered && l.second != nullptr)
            l.second();
    }
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
