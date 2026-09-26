// Flubsound FX - VST3 / AU / Standalone plug-in around flub::ProcessingChain.
//
// * One ProcessingChain with its own ParameterStore (the same engine the
//   desktop app and flubsound-cli run).
// * Every flub::param::layout() entry is exposed as a host parameter through
//   an AudioProcessorValueTreeState: parameter ID = Info::key (versioned
//   ParameterID, version hint = Info::sinceVersion), floats with
//   NormalisableRange + skew from Info::skewCentre, AudioParameterChoice for
//   choices, AudioParameterBool for toggles. The "bypass" parameter is also the host bypass parameter, so
//   host bypass is the chain's click-free, latency-compensated bypass.
// * processBlock copies the APVTS raw values into the store (plain atomic
//   loads/stores, only for values that changed - RT-safe), holds
//   ScopedNoDenormals and runs the chain in chunks of at most the prepared
//   block size (hosts may exceed the size announced in prepareToPlay).
// * Latency: setLatencySamples (chain latency) in prepareToPlay. Structural
//   parameters (every Info::structural entry; today the latency profile): a
//   message-thread timer notices a change, suspends processing, re-prepares
//   the chain and reports the new latency to the host.
// * Buses: stereo -> stereo, mono -> stereo (duplicated), 5.1 / 7.1 -> stereo
//   (binaural virtualiser or ITU downmix inside the chain). Surround channels
//   are re-ordered from JUCE's channel order into the chain's
//   WAVE_FORMAT_EXTENSIBLE order (FL FR FC LFE [BL BR] SL SR) by pointer
//   mapping - no copies.
// * State: APVTS XML (getStateInformation / setStateInformation) plus import
//   and export of Flubsound preset JSON (the app / CLI preset format).
//   As in the app, "Bypass All" is never taken from or written to a preset.
#pragma once

#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace flub::plugin
{
class FlubsoundProcessor final : public juce::AudioProcessor,
                                 private juce::Timer
{
public:
    FlubsoundProcessor();
    ~FlubsoundProcessor() override;

    //==============================================================================
    const juce::String getName() const override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;         // double precision: not supported
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorParameter* getBypassParameter() const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Flubsound specifics (message thread)
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }

    /** Telemetry published by the audio thread (atomics; poll from a timer). */
    const flub::MeterBus& getMeters() noexcept { return chain.meters(); }

    /** Loads a Flubsound preset JSON (*.flubpreset.json) into the parameters. */
    bool importPreset (const juce::File& file, juce::String& error);

    /** Writes the current parameters as a Flubsound preset JSON. */
    bool exportPreset (const juce::File& file, juce::String& error) const;

    /** The chain's latency in samples as last reported to the host. */
    int getChainLatencySamples() const noexcept { return reportedLatency.load (std::memory_order_relaxed); }

    /** The parameter layout (public so tests / tools can inspect it). */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    juce::AudioProcessorEditor* createEditor() override;
    void timerCallback() override;

    /** Copies changed APVTS raw values into the store. RT-safe. */
    void pushParametersToStore() noexcept;
    /** (Re-)prepares the chain for the current layout / structural params. Non-RT. */
    void prepareChain (double sampleRate, int maxBlockSize);
    /** Any Info::structural parameter differs from what the chain was prepared
        with. Message thread, prepareMutex held. */
    bool structuralParameterChanged() const noexcept;
    void processInternal (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept;

    flub::param::ParameterStore store;
    flub::ProcessingChain chain { store };
    juce::AudioProcessorValueTreeState apvts;

    // Audio-thread view of the APVTS (pointers are stable for the lifetime of the APVTS).
    std::array<std::atomic<float>*, flub::param::kNumParams> rawValues {};
    // Cached: wrappers (VST3, VST2, AAX, AudioProcessorGraph) call
    // getBypassParameter() on the audio thread for every block, so it must
    // not build a String and search the APVTS there.
    juce::AudioProcessorParameter* bypassParameter = nullptr;
    std::array<float, flub::param::kNumParams> lastPushed {}; // audio thread (or while suspended)

    // Prepared configuration (written while processing is stopped / suspended).
    std::array<int, flub::kMaxChannels> channelMap {}; // chain channel -> buffer channel
    int chainInputChannels = 2;
    bool duplicateMono = false;
    double preparedSampleRate = 48000.0;
    int preparedBlockSize = 0;
    // Store values the chain was last prepared with (only the structural ones
    // are compared). Guarded by prepareMutex (prepareToPlay may run off the message thread).
    std::array<float, flub::param::kNumParams> preparedValues {};
    std::atomic<bool> prepared { false };
    std::atomic<int> reportedLatency { 0 };
    std::mutex prepareMutex; // prepareToPlay / releaseResources vs. the re-prepare timer (never on the audio thread)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlubsoundProcessor)
};
} // namespace flub::plugin
