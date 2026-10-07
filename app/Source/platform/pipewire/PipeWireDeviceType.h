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
// Built with libpipewire's headers only (FLUB_HAS_PIPEWIRE); elsewhere
// addDeviceType adds nothing, so a caller needs no #if. The library is opened
// at run time (R1.2, PipeWireLibrary.h): where it does not load,
// addDeviceType adds nothing either; serverPlaysAudio says whether a first
// start should prefer the node (a server that plays audio). The device
// reports the node's xruns
// (getXRunCount), passes the driver's time as hostTimeNs, and hands a node
// error (server gone, node removed) to its callback's audioDeviceError on the
// message thread.
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

/** R1.2: where a running PipeWire device reports that its node stopped
    working (server gone, node removed): 'target'->audioDeviceError on the
    message thread. JUCE 9.0.2's AudioDeviceManager starts devices through a
    wrapper (CallbackMaxSizeEnforcer) that does not forward audioDeviceError,
    so an error given to the device's own callback never reaches the
    manager's callbacks; the host registers itself here instead (from
    audioDeviceAboutToStart). Cleared when the device stops; nullptr clears.
    false when 'device' is not a PipeWire device. Any thread. */
bool setDeviceErrorTarget (juce::AudioIODevice* device, juce::AudioIODeviceCallback* target);

/** The node error a PipeWire device holds for the message thread, not yet
    handed to its error target; empty when none (or not a PipeWire device).
    Closing the device drops it (a re-open is a new run). Tests. */
juce::String getPendingDeviceError (juce::AudioIODevice* device);

/** R1.2: whether a PipeWire server answers and plays the desktop's audio
    (an output sink that is not Flubsound's own, pipewire::playsAudio):
    AudioEngineHost's first start prefers the node only then, so a PipeWire
    run for screen capture beside PulseAudio, or libpipewire installed on a
    PulseAudio desktop, keeps ALSA. false with 'why' otherwise (library
    missing, no server, no audio output). Connects and disconnects a probe
    session (a few ms). Message thread. */
bool serverPlaysAudio (std::string& why);
#else
inline constexpr bool kHasDeviceType = false;
inline bool addDeviceType (juce::AudioDeviceManager&) { return false; }
inline bool setDeviceLatency (juce::AudioIODevice*, NativeAudioNodeConfig::Latency) { return false; }
inline bool setDeviceOutputTarget (juce::AudioIODevice*, const std::string&) { return false; }
inline NativeAudioNodeStatus getDeviceStatus (juce::AudioIODevice*) { return {}; }
inline bool setDeviceErrorTarget (juce::AudioIODevice*, juce::AudioIODeviceCallback*) { return false; }
inline juce::String getPendingDeviceError (juce::AudioIODevice*) { return {}; }
inline bool serverPlaysAudio (std::string& why)
{
    why = "this build has no PipeWire node";
    return false;
}
#endif

inline constexpr const char* kDeviceTypeName = "PipeWire";
inline constexpr const char* kDeviceName = "Flubsound Engine";             // the output side (and the device)
inline constexpr const char* kInputDeviceName = "Flubsound Engine inputs"; // the strips' sinks: another name, since
                                                                           // AudioEngineHost::isLoopbackPair takes one
                                                                           // virtual device on both ends for a loop,
                                                                           // and the node never plays into its own sinks
} // namespace flub::platform::pipewire
