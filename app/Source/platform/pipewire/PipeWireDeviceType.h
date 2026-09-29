// Flubsound Pro - the native PipeWire node as a JUCE device type (docs/11 E48).
//
// "PipeWire" in the device list, with one device, "Flubsound Engine" (its
// input side is listed as "Flubsound Engine inputs"): a
// NativeAudioNode whose inputs are the four strips' sink monitors (Game 7.1,
// Music, Chat, System: 14 input channels named "Game:FL" ... "System:FR",
// which AudioEngineHost::positionFromChannelName reads) and whose outputs
// are linked to the default output (or the sink named by setOutputTarget).
// The engine then runs inside the pw_filter's real-time process callback,
// with no JACK client, no monitor links to set up by hand and no 14-port
// input device needed. The callback swap is lock-free (an atomic pointer and
// a busy flag; stop() waits on the message thread, never the audio thread).
//
// Built with libpipewire only (FLUB_HAS_PIPEWIRE); elsewhere addDeviceType
// adds nothing, so a caller needs no #if.
#pragma once

#include "../PlatformServices.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace flub::platform::pipewire
{
#if defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE
inline constexpr bool kHasDeviceType = true;

/** The "PipeWire" device type (a new object each call). */
std::unique_ptr<juce::AudioIODeviceType> createDeviceType();

/** Adds the type to 'manager' after JUCE's own types. JUCE creates its
    default types only while the list is empty, so the list is filled first
    (getAvailableDeviceTypes) and the default device type stays JUCE's.
    Returns true when added. Message thread, before initialise(). */
bool addDeviceType (juce::AudioDeviceManager& manager);

/** Changes a running PipeWire device's node.latency in place (no re-open):
    the device type's answer to EngineController's latency profile. false
    when 'device' is not a PipeWire device. */
bool setDeviceLatency (juce::AudioIODevice* device, NativeAudioNodeConfig::Latency latency);

/** Plays a running PipeWire device to the sink named 'sinkName' (empty =
    the default output) by moving its output links; false when 'device' is
    not a PipeWire device. The choice is kept for the next open(). */
bool setDeviceOutputTarget (juce::AudioIODevice* device, const std::string& sinkName);

/** The node's link status (routing panel); running == false when 'device'
    is not a PipeWire device. */
NativeAudioNodeStatus getDeviceStatus (juce::AudioIODevice* device);
#else
inline constexpr bool kHasDeviceType = false;
inline bool addDeviceType (juce::AudioDeviceManager&) { return false; }
inline bool setDeviceLatency (juce::AudioIODevice*, NativeAudioNodeConfig::Latency) { return false; }
inline bool setDeviceOutputTarget (juce::AudioIODevice*, const std::string&) { return false; }
inline NativeAudioNodeStatus getDeviceStatus (juce::AudioIODevice*) { return {}; }
#endif

inline constexpr const char* kDeviceTypeName = "PipeWire";
inline constexpr const char* kDeviceName = "Flubsound Engine";             // the output side (and the device)
inline constexpr const char* kInputDeviceName = "Flubsound Engine inputs"; // the strips' sinks: another name, since
                                                                           // AudioEngineHost::isLoopbackPair takes one
                                                                           // virtual device on both ends for a loop,
                                                                           // and the node never plays into its own sinks
} // namespace flub::platform::pipewire
