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
// Footer: "Assign app to strip..." (running audio sessions from AppRouting,
// or a typed executable name) for the selected strip and "System sound
// settings" (the OS's per-app audio device page).
// When per-app routing is unavailable (no platform services, unsupported OS
// version, or switched off) the assign button is greyed out and the panel
// explains why and what to do instead (a one-line notice with the full text
// on hover / click when the panel is short).
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

private:
    class StripRow;

    void showAssignMenu();
    void promptForExecutable (const juce::String& stripName);
    void showChipMenu (const juce::String& executable, const juce::String& error);
    juce::String unsupportedReason() const;
    juce::TextLayout layoutReason (int width) const;

    EngineController& controller;
    juce::Component rowHolder;
    juce::Viewport rowView;
    std::vector<std::unique_ptr<StripRow>> rows;
    IconButton assignButton { "Assign an application to the selected strip", Icons::plus(), IconButton::Style::Framed };
    IconButton systemButton { "Open system routing settings", Icons::external(), IconButton::Style::Framed };
    juce::String reason;
    juce::Rectangle<int> headerArea, reasonArea;
    bool reasonCompact = false; // one-line notice (full text on hover / click) when space is short
    int selectedStrip = -1;
};
} // namespace flub::app::ui
