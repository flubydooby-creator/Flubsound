// Flubsound Pro - the personal hearing profile's editor (docs/11 E33 slice;
// Settings > Hearing).
//
// A LISTENING PREFERENCE entered by hand, not a hearing test, a fitting or a
// hearing aid (flub::PersonalProfile): an on / off switch, a balance (it
// only ever turns one ear down) and, per ear, a broadband gain (+-12 dB) and
// gains at 250 Hz .. 8 kHz (+-15 dB). Every slider's value box takes typed
// numbers (manual entry). Each edit goes to EngineController::
// setPersonalProfile at once: every strip's chain crossfades to it (20 ms)
// and the controller writes the profile's file a moment later. Under the
// sliders a line says what the chain makes of it: each ear's target is at
// most +15 dB and the ears at most 12 dB apart (the core's caps), and both
// ears are turned down by the headroom reservation (so the boosted ear does
// not drive the stereo-linked dynamics), which the maximizer's drive or the
// volume gives back.
#pragma once

#include "engine/EngineController.h"

#include "flub/engine/PersonalProfile.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace flub::app::ui
{
class PersonalProfileEditor : public juce::Component
{
public:
    explicit PersonalProfileEditor (EngineController& controller);

    /** Shows the controller's profile (after an edit elsewhere) and the
        selected strip's reservation. Cheap when nothing changed. */
    void refresh();

    /** The line under the sliders: "Off: ...", "Flat: ...", or each ear's
        broadband level after the caps and the reservation both ears get
        (reservationDb <= 0, ProcessingChain::getPersonalReservationDb). */
    static juce::String describeProfile (const flub::PersonalProfile& profile, float reservationDb);

    /** Height the editor needs (it lays itself out top to bottom). */
    static constexpr int kPreferredHeight = 416;

    // Controls (tests, screenshots).
    juce::ToggleButton& getEnableToggle() noexcept { return enableToggle; }
    juce::Slider& getBalanceSlider() noexcept { return balance; }
    juce::Slider& getGainSlider (int ear) noexcept { return gain[static_cast<size_t> (ear == 0 ? 0 : 1)]; }
    juce::Slider& getBandSlider (int ear, int band) noexcept
    {
        return bands[static_cast<size_t> (ear == 0 ? 0 : 1)][static_cast<size_t> (juce::jlimit (0, flub::PersonalProfile::kNumBands - 1, band))];
    }
    juce::TextButton& getFlatButton() noexcept { return flatButton; }
    const juce::String& getDescription() const noexcept { return description; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void edited();
    void showProfile (const flub::PersonalProfile& profile);
    static juce::String bandName (int band);

    EngineController& controller;
    juce::ToggleButton enableToggle { "Use my personal profile (per ear)" };
    juce::Slider balance;
    std::array<juce::Slider, 2> gain;
    std::array<std::array<juce::Slider, flub::PersonalProfile::kNumBands>, 2> bands;
    juce::TextButton flatButton { "Flat" };
    flub::PersonalProfile shown;
    bool updating = false;
    float shownReservationDb = 0.0f;
    juce::String description;
    juce::Rectangle<int> introArea, balanceCaption, gridHeader, rowCaptions[2], descriptionArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PersonalProfileEditor)
};
} // namespace flub::app::ui
