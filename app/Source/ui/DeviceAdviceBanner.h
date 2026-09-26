// Flubsound Pro - headset / output-device advice banner.
//
// A slim strip under the header that surfaces the device profile match
// (EngineController::getDeviceAdvice): the recognised headset family or
// Bluetooth connection, the safety ceiling applied to the master limiter,
// the most important piece of setup advice (e.g. "turn off Superhuman
// Hearing"), a one-click button for the suggested preset and a link to the
// full guidance in Settings > Audio. It is shown only when there is something
// device-specific to say (a matched profile, or a Bluetooth / hands-free
// connection) and can be dismissed per output device for the session.
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
class DeviceAdviceBanner final : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    explicit DeviceAdviceBanner (EngineController& controller);

    /** Re-reads the controller's device state. Returns true when the banner's
        visibility changed (the owner then re-runs its layout). */
    bool refresh();
    bool shouldShow() const noexcept { return showing; }

    static constexpr int kHeight = 34;

    std::function<void()> onDetailsRequested;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applySuggestedPreset();

    EngineController& controller;
    juce::TextButton presetButton, detailsButton { "Details" }, dismissButton;
    juce::String headline, advice, dismissedFor, deviceName, suggestedPreset;
    bool showing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceAdviceBanner)
};
} // namespace flub::app::ui
