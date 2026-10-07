// Flubsound Pro - banners under the header that need the user's attention.
//
// DeviceErrorBanner  a device problem (docs/11 E51 Phase A;
//                    EngineController::getDeviceSafetyState): the loopback
//                    guard holding the output at silence, a device error or a
//                    device that failed to open, with the host's message and
//                    three actions - Retry (EngineController::retryDevice),
//                    Choose output (Settings > Audio) and Sound settings (the
//                    system's own sound / routing settings). Shown while the
//                    state is not None; it cannot be dismissed, because the
//                    output is silent or gone until the problem is fixed.
//                    Also, in amber, while the chosen output is missing and
//                    another plays (docs/11 E51 outputFallback: which device,
//                    and the safe speaker profile when it is on); it goes
//                    when the chosen output is back.
// NoticeBar          one line of notices, the newest on top of a short queue
//                    (a notice with the same key replaces the older one):
//                    * preset reader warnings (docs/11 E52: unknown keys,
//                      clamped values, a newer minor version) of a preset
//                      loaded or imported - a toast that hides after 12 s;
//                    * a damaged settings file that was quarantined and
//                      restored from a backup (AppSettings::getRecovery) -
//                      stays until dismissed;
//                    * the latency-profile suggestion (docs/11 E42a): a
//                      loaded preset was made for another profile; its
//                      button switches the profile (every strip), the
//                      preset never does by itself;
//                    * hotkeys that are not active (R4.4: could not be
//                      registered, the same chord as another action, ...),
//                      posted once per action and chord (HotkeyManager::
//                      takeUnannouncedFailures), listing every hotkey still
//                      failing and kept up to date (refresh); "Fix in
//                      Settings" opens Settings > Hotkeys; it goes by itself
//                      when they work.
// Both are message-thread components owned by MainComponent, which lays them
// out when refresh() / onVisibilityChanged say their visibility changed.
#pragma once

#include "engine/EngineController.h"
#include "shell/HotkeyManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>
#include <functional>
#include <vector>

namespace flub::app::ui
{
class DeviceErrorBanner final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    explicit DeviceErrorBanner (EngineController& controller);

    /** Re-reads the controller's device safety state. Returns true when the
        banner's visibility changed (the owner then re-runs its layout). */
    bool refresh();
    bool shouldShow() const noexcept { return showing; }

    /** The bold part of the banner for a state ("Output muted: feedback
        loop", "Audio device error", "Output fallback"); empty for Kind::None
        without a fallback. */
    static juce::String headlineFor (const DeviceSafetyState& state);
    /** The message: the host's text, or a fallback when it has none. */
    static juce::String messageFor (const DeviceSafetyState& state);

    static constexpr int kHeight = 34;

    /** Choose output: the owner opens Settings > Audio. */
    std::function<void()> onChooseOutput;
    /** Sound settings: the owner opens the system's sound settings. */
    std::function<void()> onOpenSoundSettings;

    juce::Button& getRetryButton() noexcept { return retryButton; }
    juce::Button& getChooseOutputButton() noexcept { return chooseButton; }
    juce::Button& getSoundSettingsButton() noexcept { return soundButton; }
    const juce::String& getHeadline() const noexcept { return headline; }
    const juce::String& getMessage() const noexcept { return message; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void retry();

    EngineController& controller;
    juce::TextButton retryButton { "Retry" }, chooseButton { "Choose output" }, soundButton { "Sound settings" };
    juce::String headline, message, retryError;
    bool showing = false, warnOnly = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceErrorBanner)
};

class NoticeBar final : public juce::Component,
                        public juce::SettableTooltipClient,
                        private juce::Timer
{
public:
    struct Notice
    {
        enum class Kind
        {
            Info,    // accent
            Warning, // amber
            Prompt   // accent, with an action
        };

        juce::String key;          // a notice with the same key replaces the older one
        Kind kind = Kind::Info;
        juce::String text;         // one line
        juce::String detail;       // tooltip / accessible description
        juce::String actionLabel;  // optional button
        std::function<void()> action;
        double seconds = 0.0;      // hides by itself after this long; 0 = until dismissed
    };

    NoticeBar();
    ~NoticeBar() override;

    /** Shows `notice` now (it replaces a notice with the same key). */
    void post (Notice notice);
    /** Replaces the notice with `notice`'s key where it is (keeping its place
        and its time left) without bringing it to the front; false when there
        is none (nothing is posted then). */
    bool refresh (Notice notice);
    /** Removes the notice with this key, if any. */
    void dismiss (const juce::String& key);
    void clear();
    bool hasNotice (const juce::String& key) const;
    /** The notice shown (the newest), nullptr when there is none. */
    const Notice* current() const noexcept { return entries.empty() ? nullptr : &entries.front().notice; }
    int getNumNotices() const noexcept { return static_cast<int> (entries.size()); }
    bool shouldShow() const noexcept { return ! entries.empty(); }

    /** Drops the notices whose time ran out, as of `nowMs`
        (juce::Time::getMillisecondCounterHiRes(); the bar's timer calls it). */
    void expire (double nowMs);

    /** Called when the bar appears or disappears (the owner re-lays out). */
    std::function<void()> onVisibilityChanged;

    juce::Button& getActionButton() noexcept { return actionButton; }
    juce::Button& getDismissButton() noexcept { return dismissButton; }

    // ---- The notices the app posts (pure; tested) --------------------------------------
    static constexpr double kPresetWarningSeconds = 12.0;
    static constexpr const char* kPresetWarningsKey = "preset-warnings";
    static constexpr const char* kRecoveryKey = "settings-recovery";
    static constexpr const char* kLatencyKey = "latency-suggestion";
    static constexpr const char* kHotkeysKey = "hotkey-failures";

    /** "Preset "Club Loud": unknown parameter "bost" ignored (did you mean
        "boost"?) (+1 more)"; the detail lists every warning. */
    static Notice presetWarningsNotice (const EngineController::PresetWarnings& warnings);
    /** The settings file was damaged: which backup came back, or that the
        defaults did, and where the damaged file was kept. Kind None if the
        file was not damaged (text empty). */
    static Notice recoveryNotice (const AppSettings::Recovery& recovery);
    /** ""Competitive FPS" was made for Low Latency; the engine runs Balanced."
        with the button "Use Low Latency", which calls `accept`. */
    static Notice latencyNotice (const EngineController::LatencySuggestion& suggestion, std::function<void()> accept);
    /** "Hotkey not active: Bypass hotkey strip (Ctrl+Alt+B) could not be
        registered (another application may hold it, or the system reserves
        it)." (+1 other), a warning that stays until dismissed or fixed, with
        "Fix in Settings", which calls `openSettings`; the detail lists every
        failure. The text names the first failure of the list. Text empty for
        no failures. */
    static Notice hotkeyNotice (const std::vector<HotkeyManager::Failure>& failures, std::function<void()> openSettings);
    static juce::String profileName (flub::param::LatencyProfileValue profile);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Entry
    {
        Notice notice;
        double expiresMs = 0.0; // 0 = never
    };

    void timerCallback() override;
    void update (bool wasShowing);

    std::deque<Entry> entries; // newest first
    juce::TextButton actionButton, dismissButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoticeBar)
};
} // namespace flub::app::ui
