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
// For a profile whose headset has its own enhancement (Profile::onboardDsp,
// docs/11 E16) it asks, until the user answers for that output endpoint,
// whether that enhancement (Superhuman Hearing, on-board EQ) is on; "Yes"
// caps Footsteps / Detail and the virtualiser on every strip
// (EngineController::setOnboardEnhancement), "No" stores the answer. The
// same switch is on the Settings > Audio page.
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

    /** "wired", "USB / wireless dongle", "Bluetooth", "Bluetooth hands-free";
        empty for Connection::Unknown. */
    static juce::String connectionText (flub::device::Connection connection);

    std::function<void()> onDetailsRequested;

    /** True while it asks whether the headset's own enhancement is on. */
    bool isAskingEnhancement() const noexcept { return askingEnhancement; }
    juce::TextButton& getEnhancementOnButton() noexcept { return enhancementOnButton; }
    juce::TextButton& getEnhancementOffButton() noexcept { return enhancementOffButton; }
    const juce::String& getAdviceText() const noexcept { return advice; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applySuggestedPreset();
    void answerEnhancement (bool on);

    EngineController& controller;
    juce::TextButton presetButton, detailsButton { "Details" }, dismissButton;
    juce::TextButton enhancementOnButton { "Yes, it is ON" }, enhancementOffButton { "No" };
    juce::String headline, advice, dismissedFor, deviceName, suggestedPreset;
    bool showing = false, askingEnhancement = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceAdviceBanner)
};
} // namespace flub::app::ui
