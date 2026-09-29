// Flubsound Pro - the Simple view's status row (docs/11 E39).
//
//   +-- OUTPUT ----------------------------------+  +-- LOUDNESS -------------------+
//   | [ear] Turtle Beach Stealth series          |  | -12.3 LUFS                    |
//   | Headphones (Stealth 700) . USB . -1.0 dBTP |  | [=========|=====        ]     |
//   | Headphone correction: HD 600.txt (on)      |  | 2.9 LU louder than the input  |
//   | advice (only while the banner is hidden)   |  |                               |
//   |              [Headphone correction] [Output]|  |                               |
//   +--------------------------------------------+  +-------------------------------+
//   Spectrum, EQ, routing, the module rack ... are in the Advanced view.  [Advanced view]
//
// OUTPUT      the headset status: the matched profile (or the plain device
//             name: a generic output), the connection, the safety ceiling the
//             master limiter applies, the device correction (docs/11 E15) and
//             a device problem (DeviceSafetyState); buttons open Settings on
//             the Audio or the Correction page.
// LOUDNESS    the one loudness meter: the output's short-term loudness as a
//             bar (-36 .. 0 LUFS) with the input's as a marker, and in plain
//             words what the strip does to it.
// The rest of the window (BoostPanel in its Simple layout, the header, the
// banners) is laid out by MainComponent. Message thread only.
#pragma once

#include "MeterSnapshot.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
class SimpleStatusPanel final : public juce::Component
{
public:
    explicit SimpleStatusPanel (EngineController& controller);

    /** Re-reads the output device, its profile, correction and safety state.
        adviceBannerShown: the DeviceAdviceBanner above shows the advice, so
        this panel does not repeat it. */
    void refreshDevice (bool adviceBannerShown);
    /** Per display frame; repaints the loudness card at <= 10 Hz. */
    void update (const MeterSnapshot& snapshot, double dtSeconds);

    // ---- Readouts (pure; tested) -------------------------------------------------------
    struct OutputStatus
    {
        juce::String title;      // "Turtle Beach Stealth series", the device name, or "No output device"
        juce::String detail;     // "Headphones (Stealth 700) . USB / wireless dongle . ceiling -1.0 dBTP"
        juce::String correction; // "Headphone correction: HD 600.txt (on)" / "No headphone correction"
        juce::String advice;     // first advice message (empty when shown elsewhere)
        bool recognised = false; // a headset profile matched
        bool problem = false;    // the device failed or the output is muted
    };
    static OutputStatus describeOutput (const juce::String& deviceName, const juce::String& profileName,
                                        flub::device::Connection connection, const flub::device::Advice& advice,
                                        const EngineController::DeviceCorrectionInfo& correction, const DeviceSafetyState& safety,
                                        bool adviceShownElsewhere);

    /** "2.9 LU louder than the input", "1.4 LU quieter than the input",
        "About as loud as the input" (|out - in| < 0.5 LU), or "No audio on
        this strip right now" (inactive, or either side at or below -70 LUFS). */
    static juce::String describeLoudnessChange (float inLufs, float outLufs, bool active);

    /** Bar range of the loudness meter. */
    static constexpr float kMeterMinLufs = -36.0f, kMeterMaxLufs = 0.0f;

    const OutputStatus& getOutputStatus() const noexcept { return output; }
    juce::String getLoudnessText() const { return describeLoudnessChange (shown.in, shown.out, shown.active); }
    juce::Button& getAdvancedButton() noexcept { return advancedButton; }

    std::function<void()> onOutputSettingsRequested, onCorrectionRequested, onAdvancedRequested;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Shown
    {
        float in = -160.0f, out = -160.0f;
        bool active = false;
    };

    EngineController& controller;
    juce::TextButton outputButton { "Output..." }, correctionButton { "Headphone correction..." }, advancedButton { "Advanced view" };
    OutputStatus output;
    Shown shown, painted;
    float sinceRepaint = 0.0f;
    juce::Rectangle<int> outputCard, loudnessCard, footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SimpleStatusPanel)
};
} // namespace flub::app::ui
