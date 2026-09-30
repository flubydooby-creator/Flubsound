// Flubsound Pro - strips and per-application routing.
//
// One row per strip (Game 7.1 / Music / Chat / System by default):
//   activity LED, name, channel badge, mute, gain (-60 .. +12 dB), a stereo
//   mini peak meter of the strip output (IEC 60268-18 deflection, -70 .. 0
//   dBFS, like LevelMeters) and the applications routed to it (chips: click
//   for "move to strip" / "remove"). A chip's state is drawn as a shape as
//   well as a colour (playing / idle / not running / error) and named in its
//   tooltip; a routing or capture error shows its text in the tooltip and as
//   an entry of the chip's menu. Clicking a row selects the strip for
//   editing (same as the header's strip selector).
// The Chat strip's row (docs/11 E22) adds a voice-chat line: the ChatMix
// balance (EngineController::setChatMix, as the tray flyout and the ChatMix
// hotkeys), a dot that lights while the strip carries speech, the "Duck game
// under voice chat" switch and its depth (3 - 6 dB, persisted).
// Footer: "Assign app to strip..." (running audio sessions from AppRouting,
// or a typed executable name) for the selected strip and "System sound
// settings" (the OS's per-app audio device page).
// While no application is processed through per-app routing (nothing moved
// to a strip endpoint or captured: none assigned, none running, every one
// failing, or routing unavailable) a red "No apps are being processed" notice
// sits under the header and says why. When per-app routing is unavailable
// (no platform services, a Windows build that cannot move apps and has no
// process capture, or switched off) the assign button is also greyed out and
// the notice says what to do instead. When the panel is short the notice is
// one line, with the full text on hover / click.
// Doubling guard (docs/11 E47): an assigned app that plays straight to the
// device Flubsound plays to is not captured (AppRouting holds it back, it
// would be heard twice). Its chip carries an amber "original also audible"
// badge (two overlapping rings) and its menu the fix; while processing
// works otherwise, an amber notice under the header names the apps and the
// fix (set the app's output to another device, which it lists).
//
// Below the strips, "Auto profiles" (roadmap 2.5): one line per rule
// ("cs2 -> Game: Competitive FPS, restores on exit") with a remove button, a
// switch for the whole feature and "Add automatic profile..." (application
// from the recently focused / audio-playing apps or typed, strip, preset,
// mode, keep or restore on exit). The active rule or the last error is shown
// under the list; where the foreground app cannot be detected (Wayland, no
// platform services) the reason is shown instead and adding is disabled.
//
// Above the footer, one line says how the strips' endpoints reach
// Flubsound's input (docs/11 E48): with the native PipeWire device its links,
// output device and graph quantum ("PipeWire: Linked 14 of 14 input
// channels, output to ... (2 of 2), quantum 256/48000"); otherwise only
// what the router could not link (e.g. a missing Flubsound sink). The full
// text is its tooltip; amber when a link is missing.
//
// While visible the panel asks AppRouting for live session updates.
#pragma once

#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace flub::app::ui
{
/** The fields of "Add automatic profile...": the application (typed, or
    picked from the recently focused / audio-playing ones), the strip, the
    preset, the mode and whether the previous preset returns when the
    application leaves. Hosted by the panel's dialog; usable on its own
    (tests fill it without a window). */
class AutoProfileForm : public juce::Component
{
public:
    explicit AutoProfileForm (EngineController& controller);

    /** The rule as filled in; an empty executable or preset when incomplete. */
    AutoProfileRule getRule() const;
    /** Applications offered in the "Recent" list (newest first). */
    const juce::StringArray& getSuggestions() const noexcept { return suggestions; }

    void resized() override;

    juce::TextEditor application;
    juce::ComboBox recent, strip, preset, mode;
    juce::ToggleButton restore { "Restore the previous preset when it leaves" };

private:
    EngineController& controller;
    juce::StringArray suggestions;
    std::vector<juce::String> presetIds; // item id - 1
    juce::Label applicationLabel, recentLabel, stripLabel, presetLabel, modeLabel;
};

class RoutingPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit RoutingPanel (EngineController& controller);
    ~RoutingPanel() override;

    /** Strip layout changed: rebuild the rows. */
    void rebuildStrips();
    /** Routes / running apps / capabilities changed. */
    void refreshRouting();
    /** Per display frame: meters, activity, gain / mute state. */
    void updateMeters (double dtSeconds);
    void setSelectedStrip (int strip);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

    /** The red "No apps are being processed" state is shown. */
    bool isShowingNoAppsProcessed() const noexcept { return noAppsProcessed; }
    /** The notice's full text (empty = no notice). */
    const juce::String& getNotice() const noexcept { return notice; }
    /** The amber doubling notice is shown (docs/11 E47). */
    bool isShowingDoubling() const noexcept { return doubling; }
    /** Screen-reader description of a strip row (its apps and their states). */
    juce::String getStripDescription (int strip) const;
    /** The Chat row's controls (docs/11 E22); nullptr without a Chat strip. */
    juce::Slider* getChatMixSlider();
    juce::Button* getChatDuckButton();
    juce::Slider* getChatDuckDepthSlider();
    /** The Chat row's voice dot is lit (updateMeters polls it). */
    bool isVoiceDotLit() const;
    /** Re-reads ChatMix and the duck setting into the Chat row. */
    void refreshChat();
    /** docs/11 E48: the input-link line (empty = not shown) and whether it
        reports a missing link. Re-read by refreshRouting() and, twice a
        second, by updateMeters(). */
    const juce::String& getLinkStatus() const noexcept { return linkStatus; }
    bool isLinkStatusWarning() const noexcept { return linkWarning; }
    void refreshLinkStatus();

private:
    class StripRow;
    class AutoProfileList;

    void showAssignMenu();
    void promptForExecutable (const juce::String& stripName);
    void showChipMenu (const juce::String& executable, const juce::String& error, bool doubled);
    void showDoublingFix();
    void showAddAutoProfileDialog();
    juce::TextLayout layoutNotice (int width) const;
    StripRow* findChatRow() const;

    EngineController& controller;
    juce::Component rowHolder;
    juce::Viewport rowView;
    std::vector<std::unique_ptr<StripRow>> rows;
    std::unique_ptr<AutoProfileList> autoProfiles;
    IconButton assignButton { "Assign an application to the selected strip", Icons::plus(), IconButton::Style::Framed };
    IconButton systemButton { "Open system routing settings", Icons::external(), IconButton::Style::Framed };
    juce::String reason;   // why per-app routing is unavailable (AppRouting::getUnavailableReason)
    juce::String notice;   // text of the notice under the header: the red state's or `reason`
    juce::String noticeDetail; // the red state's explanation (below its title)
    bool noAppsProcessed = false;
    bool doubling = false;     // the amber doubling notice (not while the red state shows)
    juce::Rectangle<int> headerArea, noticeArea;
    bool noticeCompact = false; // one-line notice (full text on hover / click) when space is short
    int selectedStrip = -1;
    juce::String linkStatus;     // docs/11 E48: the input-link line
    bool linkWarning = false;
    juce::Rectangle<int> linkArea;
    double linkPollSeconds = 0.0;
};
} // namespace flub::app::ui
