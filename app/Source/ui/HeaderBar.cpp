#include "HeaderBar.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr int kModeRadioGroup = 0x10de;
constexpr int kStripRadioGroup = 0x5171;
constexpr int kBankRadioGroup = 0xab;

const juce::String kDot { juce::CharPointer_UTF8 (" \xc2\xb7 ") };
} // namespace

// =============================================================================
// Mode segment: icon + label, drawn over the sliding thumb painted by the header.
// =============================================================================
class HeaderBar::ModeSegment : public juce::Button
{
public:
    ModeSegment (const juce::String& name, Icon i)
        : juce::Button (name), icon (std::move (i))
    {
        setClickingTogglesState (true);
        setRadioGroupId (kModeRadioGroup, juce::dontSendNotification);
        setMouseClickGrabsKeyboardFocus (false);
        setTitle (name + " mode");
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool) override
    {
        const auto on = getToggleState();
        const auto colour = on ? Palette::background : (isHighlighted ? Palette::text : Palette::muted);
        auto r = getLocalBounds().toFloat();
        const float h = r.getHeight();

        g.setFont (Theme::font (juce::jmin (13.0f, h * 0.44f), true));
        const auto label = getButtonText();
        const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label);
        const float iconSize = h * 0.5f;
        const float total = iconSize + 7.0f + tw;
        auto content = r.withSizeKeepingCentre (juce::jmin (total, r.getWidth() - 8.0f), h);

        drawIcon (g, icon, content.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize), colour, 1.6f);
        content.removeFromLeft (7.0f);
        g.setColour (colour);
        g.drawText (label, content, juce::Justification::centredLeft, false);

        if (hasKeyboardFocus (false))
        {
            g.setColour (Theme::accent (*this).withAlpha (0.7f));
            g.drawRoundedRectangle (r.reduced (2.0f), h * 0.5f - 2.0f, 1.2f);
        }
    }

private:
    Icon icon;
};

// =============================================================================
// Bypass button: right-click opens its options instead of toggling.
// =============================================================================
class HeaderBar::PopupButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void()> onPopupMenu;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onPopupMenu != nullptr)
                onPopupMenu();
            return;
        }
        juce::TextButton::mouseDown (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            juce::TextButton::mouseUp (e);
    }
};

// =============================================================================
HeaderBar::HeaderBar (EngineController& c)
    : controller (c)
{
    setTitle ("Flubsound Pro toolbar");

    // ---- Mode ----
    musicSegment = std::make_unique<ModeSegment> ("Music", Icons::musicNote());
    gamingSegment = std::make_unique<ModeSegment> ("Gaming", Icons::gamepad());
    musicSegment->setTooltip ("Music mode: punch, width, clarity, loudness and warmth macros");
    gamingSegment->setTooltip ("Gaming mode: footsteps, positional, impact, detail and voice macros");
    musicSegment->onClick = [this] { controller.setMode (ModeValue::Music); };
    gamingSegment->onClick = [this] { controller.setMode (ModeValue::Gaming); };
    addAndMakeVisible (*musicSegment);
    addAndMakeVisible (*gamingSegment);

    // ---- Presets ----
    presetBox.setTitle ("Preset");
    presetBox.setTooltip ("Preset of the selected strip: click to search, filter and preview presets");
    presetBox.onBrowse = [this] { showPresetBrowser(); };
    presetBox.setTextWhenNothingSelected ("Default settings");
    presetBox.setTextWhenNoChoicesAvailable ("No presets installed");
    presetBox.onChange = [this]
    {
        const int item = presetBox.getSelectedId();
        if (item <= 0 || item > static_cast<int> (presetIds.size()))
            return;
        juce::String error;
        const auto id = presetIds[static_cast<size_t> (item - 1)];
        if (! controller.loadPreset (id, -1, error))
            showError ("Could not load the preset", error);
        else
            controller.getSettings().addRecentPreset (id);
    };
    addAndMakeVisible (presetBox);

    prevPreset.onClick = [this]
    {
        if (controller.previousPreset())
            controller.getSettings().addRecentPreset (controller.getCurrentPresetId());
    };
    nextPreset.onClick = [this]
    {
        if (controller.nextPreset())
            controller.getSettings().addRecentPreset (controller.getCurrentPresetId());
    };
    presetMenu.onClick = [this] { showPresetMenu(); };
    prevPreset.setTooltip ("Previous preset");
    nextPreset.setTooltip ("Next preset");
    presetMenu.setTooltip ("Browse, save, rename, delete, import or export presets");
    addAndMakeVisible (prevPreset);
    addAndMakeVisible (nextPreset);
    addAndMakeVisible (presetMenu);

    // ---- Strip menu (narrow windows) ----
    stripBox.setTitle ("Strip");
    stripBox.setTooltip ("The strip (per-app profile) the window edits");
    stripBox.onChange = [this]
    {
        if (const int id = stripBox.getSelectedId(); id > 0)
            controller.setSelectedStrip (id - 1);
    };
    addChildComponent (stripBox);

    // ---- A / B (loudness matched, docs/11 E37) ----
    comparison = std::make_unique<BankComparison> (controller, [this] { return getLoudnessEstimator(); });
    comparison->onStatusChanged = [this] { refresh(); };
    abA = std::make_unique<PopupButton> ("A");
    abB = std::make_unique<PopupButton> ("B");
    for (auto* b : { abA.get(), abB.get() })
    {
        Style::set (*b, "tab");
        b->setRadioGroupId (kBankRadioGroup, juce::dontSendNotification);
        b->setClickingTogglesState (true);
        b->onPopupMenu = [this, b] { showCompareMenu (*b); };
        addAndMakeVisible (*b);
    }
    abA->setConnectedEdges (juce::Button::ConnectedOnRight);
    abB->setConnectedEdges (juce::Button::ConnectedOnLeft);
    abA->onClick = [this] { controller.setActiveBank (Bank::A); };
    abB->onClick = [this] { controller.setActiveBank (Bank::B); };
    copyAB.onClick = [this] { controller.copyActiveToOtherBank(); };
    addAndMakeVisible (copyAB);

    overflowButton.setTooltip ("Latency and CPU, copy A / B, the blind test, routing and settings");
    overflowButton.onClick = [this] { showOverflowMenu(); };
    addChildComponent (overflowButton);

    // ---- Bypass ----
    bypassButton = std::make_unique<PopupButton> ("Bypass");
    Style::set (*bypassButton, "warning");
    bypassButton->onClick = [this] { controller.toggleEnabled(); };
    bypassButton->onPopupMenu = [this] { showBypassMenu(); };
    addAndMakeVisible (*bypassButton);

    // ---- Settings ----
    settingsButton.setTooltip ("Audio device, latency, hotkeys and start-up settings");
    settingsButton.onClick = [this]
    {
        if (onSettingsRequested != nullptr)
            onSettingsRequested();
    };
    addAndMakeVisible (settingsButton);

    // ---- View (Simple / Advanced) ----
    viewButton.onClick = [this]
    {
        if (onViewToggleRequested != nullptr)
            onViewToggleRequested();
    };
    addAndMakeVisible (viewButton);
    setSimpleView (false);

    rebuildStrips();
    refreshPresets();
    refresh();
    updateStatus();
    thumbPos = thumbTarget;
}

HeaderBar::~HeaderBar()
{
    comparison.reset(); // before the estimator it listens to
}

std::shared_ptr<PresetLoudnessEstimator> HeaderBar::getLoudnessEstimator()
{
    // One estimator per engine rate; its estimates are kept between uses.
    const double rate = controller.getHost().getSampleRate() > 0.0 ? controller.getHost().getSampleRate() : 48000.0;
    if (loudnessEstimator == nullptr || loudnessEstimator->getSampleRate() != rate)
        loudnessEstimator = std::make_shared<PresetLoudnessEstimator> (rate);
    return loudnessEstimator;
}

juce::Button& HeaderBar::getBankButton (Bank bank) noexcept
{
    return bank == Bank::A ? *abA : *abB;
}

void HeaderBar::setComparisonMatched (bool matched)
{
    controller.getSettings().setComparisonMatched (matched);
    comparison->setEnabled (matched);
    if (onComparisonMatchedChanged != nullptr)
        onComparisonMatchedChanged (matched);
    refresh();
}

// =============================================================================
// State
// =============================================================================
void HeaderBar::setSimpleView (bool simple)
{
    viewButton.setIcon (simple ? Icons::expand() : Icons::collapse());
    Style::describe (viewButton, simple ? "Advanced view" : "Simple view",
                     simple ? "Advanced view: routing, spectrum and EQ, the module rack and every meter"
                            : "Simple view: Boost, the five macros, what is active now, your headset and one loudness meter");
}

void HeaderBar::rebuildStrips()
{
    for (auto& b : stripButtons)
        removeChildComponent (b.get());
    stripButtons.clear();

    const int n = controller.getNumStrips();
    for (int i = 0; i < n; ++i)
    {
        auto b = std::make_unique<juce::TextButton> (controller.getStripName (i));
        Style::set (*b, "tab");
        b->setRadioGroupId (kStripRadioGroup, juce::dontSendNotification);
        b->setClickingTogglesState (true);
        int edges = 0;
        if (i > 0)
            edges |= juce::Button::ConnectedOnLeft;
        if (i < n - 1)
            edges |= juce::Button::ConnectedOnRight;
        b->setConnectedEdges (edges);
        const auto channels = controller.getStripChannels (i);
        Style::describe (*b, "Edit the " + controller.getStripName (i) + " strip",
                         "Edit the " + controller.getStripName (i) + " strip (" + (channels > 2 ? juce::String (channels == 8 ? "7.1" : "5.1") : juce::String ("stereo"))
                             + "). A dot means it is receiving audio.");
        b->onClick = [this, i] { controller.setSelectedStrip (i); };
        addAndMakeVisible (*b);
        stripButtons.push_back (std::move (b));
    }
    stripBox.clear (juce::dontSendNotification);
    for (int i = 0; i < n; ++i)
        stripBox.addItem (controller.getStripName (i), i + 1);
    stripActive.assign (static_cast<size_t> (n), false);
    resized();
    refresh();
}

void HeaderBar::refreshPresets()
{
    presetBox.clear (juce::dontSendNotification);
    presetIds.clear();

    juce::String lastHeading;
    for (const auto& p : controller.getPresetManager().getPresets())
    {
        const auto heading = (p.isFactory ? "Factory - " : "User - ") + p.category;
        if (heading != lastHeading)
        {
            presetBox.addSectionHeading (heading);
            lastHeading = heading;
        }
        presetIds.push_back (p.id);
        presetBox.addItem (p.name, static_cast<int> (presetIds.size()));
    }
    refresh();
}

const PresetInfo* HeaderBar::currentPreset() const
{
    return controller.getPresetManager().findById (controller.getCurrentPresetId());
}

void HeaderBar::refresh()
{
    const auto mode = controller.getMode();
    musicSegment->setToggleState (mode == ModeValue::Music, juce::dontSendNotification);
    gamingSegment->setToggleState (mode == ModeValue::Gaming, juce::dontSendNotification);
    thumbTarget = mode == ModeValue::Gaming ? 1.0f : 0.0f;

    const int strip = controller.getSelectedStrip();
    for (size_t i = 0; i < stripButtons.size(); ++i)
        stripButtons[i]->setToggleState (static_cast<int> (i) == strip, juce::dontSendNotification);

    const auto currentId = controller.getCurrentPresetId();
    int item = 0;
    for (size_t i = 0; i < presetIds.size(); ++i)
        if (presetIds[i] == currentId)
            item = static_cast<int> (i) + 1;
    presetBox.setSelectedId (item, juce::dontSendNotification);
    if (const auto* preset = currentPreset())
        presetBox.setTooltip (preset->name + "  (" + (preset->isFactory ? "factory" : "user") + " - " + preset->category + ")");
    const bool hasPresets = ! presetIds.empty();
    prevPreset.setEnabled (hasPresets);
    nextPreset.setEnabled (hasPresets);

    stripBox.setSelectedId (strip + 1, juce::dontSendNotification);

    const auto bank = controller.getActiveBank();
    abA->setToggleState (bank == Bank::A, juce::dontSendNotification);
    abB->setToggleState (bank == Bank::B, juce::dontSendNotification);
    copyAB.setTooltip (bank == Bank::A ? "Copy A to B" : "Copy B to A");
    const auto status = comparison->getStatus (strip);
    const auto abTip = (comparison->isEnabled() ? BankComparison::describe (status)
                                                : juce::String ("A/B is not loudness matched (right-click to switch the match on)."))
                       + "\nRight-click: loudness matching, copy, blind test (A/B/X).";
    Style::describe (*abA, "Settings A", "Listen to / edit settings A. " + abTip);
    Style::describe (*abB, "Settings B", "Listen to / edit settings B. " + abTip);
    abCaption = comparison->isEnabled() ? BankComparison::shortText (status) : juce::String();

    const bool bypassed = ! controller.isEnabled();
    bypassButton->setToggleState (bypassed, juce::dontSendNotification);
    bypassButton->setButtonText (bypassed ? "Bypassed" : "Bypass");
    const bool matched = controller.getSelectedParams().get (LoudnessMatchBypass) >= 0.5f;
    bypassCaption = getBypassCaption();
    juce::String bypassTip;
    bypassTip << "Bypass every strip (" << (matched ? "loudness matched" : "not loudness matched") << ") for a fair before / after comparison.";
    if (processedDeltaKnown)
        bypassTip << " The processed sound " << (bypassed ? "was " : "is ") << formatProcessedDelta (processedDeltaLu)
                  << " against the input (short-term)" << (matched ? "; the bypass plays the input at the same loudness." : ".");
    Style::describe (*bypassButton, "Bypass all processing", bypassTip + " Right-click for options.");
    repaint();
}

juce::String HeaderBar::formatProcessedDelta (float lu)
{
    return Theme::formatSignedDb (lu, 1) + " LU";
}

juce::String HeaderBar::getBypassCaption() const
{
    if (controller.isEnabled() || ! processedDeltaKnown)
        return {};
    return "proc. " + formatProcessedDelta (processedDeltaLu);
}

juce::String HeaderBar::getAbCaption() const
{
    return abCaption;
}

void HeaderBar::trackProcessedDelta()
{
    // Processed minus input, short-term, while processing plays: what the
    // bypass takes away ("Processed +x LU", docs/11 E37).
    if (! controller.isEnabled() || controller.isStripBypassed (controller.getSelectedStrip()))
        return;
    auto& meters = controller.getChain (controller.getSelectedStrip()).meters();
    const float in = meters.inShortTermLufs.load (std::memory_order_relaxed), out = meters.shortTermLufs.load (std::memory_order_relaxed);
    if (std::isfinite (in) && std::isfinite (out) && in > -70.0f && out > -70.0f)
    {
        processedDeltaLu = out - in;
        processedDeltaKnown = true;
    }
}

HeaderBar::CpuReadout HeaderBar::formatCpuReadout (const EngineStatus& status, const OverloadWatchdog::State& overload,
                                                   const DeviceSafetyState& safety)
{
    CpuReadout r;
    if (safety.kind != DeviceSafetyState::Kind::None)
    {
        r.caption = "DEVICE";
        r.value = safety.kind == DeviceSafetyState::Kind::LoopbackPair || safety.outputMuted ? "muted" : "error";
        r.overload = true; // the hot status colour
        return r;
    }
    if (! status.deviceOpen)
    {
        r.caption = "DEVICE";
        r.value = "offline";
        return r;
    }
    r.overload = overload.overloaded;
    r.warn = status.cpuLoad > 0.7;
    r.caption = r.overload ? "OVERLOAD" : "CPU";
    r.value = juce::String (juce::roundToInt (status.cpuLoad * 100.0)) + "%";
    if (status.xruns >= 0) // only devices that report xruns themselves
        r.value << kDot << status.xruns << " xr";
    return r;
}

juce::String HeaderBar::describeCpu (const EngineStatus& status, const OverloadWatchdog::State& overload, const juce::String& loadReduction)
{
    const auto reduction = loadReduction.isNotEmpty() ? "\n" + loadReduction : juce::String();
    if (! status.deviceOpen)
        return "CPU: no audio device open" + reduction;

    juce::String t;
    t << "CPU: " << juce::roundToInt (status.cpuLoad * 100.0) << " % of the audio callback's time budget";
    if (status.xruns >= 0)
        t << "; " << status.xruns << (status.xruns == 1 ? " xrun" : " xruns") << " reported by the device since it started";
    if (overload.overloaded)
    {
        t << "\nOverload: the audio callback is running out of time (sustained load of 90 % or more, or repeated dropouts; peak "
          << juce::roundToInt (overload.peakLoad * 100.0) << " %, " << static_cast<juce::int64> (overload.episodeGlitches)
          << " dropouts). Try the Low Latency profile (Settings > Processing), which is also the cheapest, or a larger buffer "
             "(Settings > Audio).";
    }
    else if (overload.episodes > 0)
    {
        t << "\n" << static_cast<juce::int64> (overload.episodes) << (overload.episodes == 1 ? " overload" : " overloads")
          << " this session (the last peaked at " << juce::roundToInt (overload.peakLoad * 100.0) << " %)";
    }
    return t + reduction;
}

juce::String HeaderBar::formatLatencyReadout (const LatencyInfo& info)
{
    if (! info.valid)
        return "--";
    return (info.estimated ? "~" : "") + juce::String (info.totalMs + info.captureBufferMs, 1) + " ms";
}

juce::String HeaderBar::describeLatency (const LatencyInfo& info, const juce::StringArray& stripNames)
{
    const auto ms = [] (double v) { return juce::String (v, 1) + " ms"; };
    if (! info.valid)
        return "Latency: no audible path (no device running, or its output is held at silence)";

    juce::String t;
    t << "Latency" << (info.estimated ? " (estimated)" : "") << ": device in " << ms (info.deviceInputMs) << " + engine " << ms (info.engineMs)
      << " + device out " << ms (info.deviceOutputMs);
    if (info.graphQuantumMs > 0.0)
        t << " + audio graph " << ms (info.graphQuantumMs);
    if (info.captureBufferMs > 0.0)
        t << " + app capture " << ms (info.captureBufferMs);
    t << " = " << ms (info.totalMs + info.captureBufferMs);
    if (info.estimated)
        t << "\nEstimated: the device figures are what the driver reports; Bluetooth codec delay and the OS mixer are not included.";

    // A strip in a sync group is padded to the group's slowest strip (none
    // by default: MixEngine StripConfig::syncGroup).
    for (int i = 0; i < info.numStrips && i < static_cast<int> (info.strips.size()); ++i)
    {
        const auto& strip = info.strips[static_cast<size_t> (i)];
        t << "\n" << (i < stripNames.size() ? stripNames[i] : "Strip " + juce::String (i + 1)) << ": " << ms (strip.outputMs) << " in the engine";
        if (strip.paddingSamples > 0)
            t << " (its own " << ms (strip.ownMs) << " + " << ms (strip.paddingMs) << " to stay in sync with its sync group)";
    }
    return t;
}

juce::String HeaderBar::describeDeviceSafety (const DeviceSafetyState& safety)
{
    if (safety.kind == DeviceSafetyState::Kind::None)
        return {};
    juce::String t = safety.message.isNotEmpty() ? safety.message
                     : safety.kind == DeviceSafetyState::Kind::LoopbackPair
                         ? "The output device is the loopback partner of the input device: the output is held at silence."
                         : "The audio device reported an error.";
    if (safety.outputMuted && safety.kind != DeviceSafetyState::Kind::LoopbackPair)
        t << " The output is held at silence.";
    return t + " Click to open Settings.";
}

void HeaderBar::updateStatus()
{
    trackProcessedDelta();
    std::fprintf (stderr, "DBG updateStatus known=%d delta=%f enabled=%d\n", (int) processedDeltaKnown, (double) processedDeltaLu, (int) controller.isEnabled());
    if (const auto caption = getBypassCaption(); caption != bypassCaption)
        refresh();
    const auto li = controller.getLatencyInfo();
    const auto status = controller.getStatus();

    const auto newLatency = formatLatencyReadout (li);
    const auto newCpu = formatCpuReadout (status, controller.getOverloadState(), controller.getDeviceSafetyState());
    bool changed = newLatency != latencyText || newCpu.caption != cpu.caption || newCpu.value != cpu.value || newCpu.warn != cpu.warn
                   || newCpu.overload != cpu.overload;
    latencyText = newLatency;
    cpu = newCpu;

    // The readout widens while the device reports xruns (the count follows the CPU %).
    if (const bool wide = status.deviceOpen && status.xruns >= 0; wide != wideReadout)
    {
        wideReadout = wide;
        resized();
    }

    for (size_t i = 0; i < stripActive.size(); ++i)
    {
        const bool active = controller.isStripActive (static_cast<int> (i));
        changed = changed || active != stripActive[i];
        stripActive[i] = active;
    }
    const bool modified = controller.isPresetModified();
    changed = changed || modified != presetModified;
    presetModified = modified;

    if (changed)
        repaint();
}

void HeaderBar::mouseMove (const juce::MouseEvent& e)
{
    if (! readoutArea.contains (e.getPosition()))
    {
        setTooltip ({});
        return;
    }
    juce::StringArray stripNames;
    for (int i = 0; i < controller.getNumStrips(); ++i)
        stripNames.add (controller.getStripName (i));
    juce::String tip;
    if (const auto device = describeDeviceSafety (controller.getDeviceSafetyState()); device.isNotEmpty())
        tip << device << "\n";
    tip << describeLatency (controller.getLatencyInfo(), stripNames);
    tip << "\n" << describeCpu (controller.getStatus(), controller.getOverloadState(), controller.describeLoadReduction());
    const auto streams = controller.getCaptureStreams();
    if (! streams.empty())
        tip << "\nApp capture: " << SettingsDialog::describeCaptureStreams (streams).replace ("\n", "\nApp capture: ");
    tip << "\n" << SettingsDialog::describeOutputDevice (controller, 1);
    setTooltip (tip);
}

void HeaderBar::mouseUp (const juce::MouseEvent& e)
{
    // The readout leads to Settings, where the device, the buffer and the
    // latency profile are chosen (and a device problem is explained).
    if (readoutArea.contains (e.getPosition()) && e.mouseWasClicked() && onSettingsRequested != nullptr)
        onSettingsRequested();
}

void HeaderBar::animate (double dtSeconds)
{
    if (std::abs (thumbPos - thumbTarget) < 0.001f)
        return;
    const float k = 1.0f - std::exp (-static_cast<float> (dtSeconds) / 0.055f);
    thumbPos += (thumbTarget - thumbPos) * k;
    if (std::abs (thumbPos - thumbTarget) < 0.002f)
        thumbPos = thumbTarget;
    repaint (modeArea.expanded (6));
}

// =============================================================================
// Preset actions
// =============================================================================
void HeaderBar::showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message, "OK", this);
}

void HeaderBar::showPresetMenu()
{
    const auto* current = currentPreset();
    const bool isUser = current != nullptr && ! current->isFactory;

    juce::PopupMenu menu;
    menu.addSectionHeader (current != nullptr ? current->name : juce::String ("No preset loaded"));
    menu.addItem (10, "Browse presets...");
    menu.addSeparator();
    menu.addItem (1, "Save", isUser && presetModified);
    menu.addItem (2, "Save as...");
    menu.addItem (3, "Rename...", isUser);
    menu.addItem (4, "Delete", isUser);
    menu.addSeparator();
    menu.addItem (5, "Import...");
    menu.addItem (6, "Export...", current != nullptr);
    menu.addItem (7, "Show preset folder");
    menu.addSeparator();
    menu.addItem (8, "Reset strip to defaults");
    menu.addSeparator();
    menu.addItem (9, "Export / batch process audio files...", onExportRequested != nullptr);

    juce::Component::SafePointer<HeaderBar> safe (this);
    const auto currentCopy = current != nullptr ? *current : PresetInfo();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetMenu),
                        [safe, currentCopy] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto& self = *safe;
                            switch (result)
                            {
                                case 1:
                                {
                                    juce::String error;
                                    const int strip = self.controller.getSelectedStrip();
                                    if (! self.controller.getPresetManager().saveCurrent (strip, self.controller.getParams (strip), error))
                                        self.showError ("Could not save the preset", error);
                                    self.updateStatus();
                                    break;
                                }
                                case 2: self.saveAs(); break;
                                case 3: self.renamePreset (currentCopy); break;
                                case 4: self.deletePreset (currentCopy); break;
                                case 5: self.importPreset(); break;
                                case 6: self.exportPreset (currentCopy); break;
                                case 7:
                                {
                                    const auto folder = self.controller.getPresetManager().getUserPresetFolder();
                                    folder.createDirectory();
                                    folder.revealToUser();
                                    break;
                                }
                                case 8: self.resetStrip(); break;
                                case 9:
                                    if (self.onExportRequested)
                                        self.onExportRequested();
                                    break;
                                case 10: self.showPresetBrowser(); break;
                                default: break;
                            }
                        });
}

void HeaderBar::showPresetBrowser()
{
    auto* parent = getParentComponent();
    if (browserOverlay != nullptr || parent == nullptr)
        return;
    browserOverlay = std::make_unique<PresetBrowserOverlay> (controller, getLoudnessEstimator(), getBottom());
    juce::Component::SafePointer<HeaderBar> safe (this);
    // Closed asynchronously: onClose runs inside the browser's own button and key handlers.
    browserOverlay->onClose = [safe]
    {
        juce::MessageManager::callAsync ([safe]
                                         {
                                             if (safe != nullptr)
                                                 safe->closePresetBrowser();
                                         });
    };
    parent->addAndMakeVisible (*browserOverlay);
    browserOverlay->setBounds (parent->getLocalBounds());
    browserOverlay->toFront (false);
    browserOverlay->getBrowser().getSearchBox().grabKeyboardFocus();
}

void HeaderBar::closePresetBrowser()
{
    browserOverlay.reset();
    refresh();
}

void HeaderBar::saveAs()
{
    const auto* current = currentPreset();
    auto* window = new juce::AlertWindow ("Save preset", "Saves the current settings of the " + controller.getStripName (controller.getSelectedStrip())
                                                             + " strip as a user preset.",
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", current != nullptr ? current->name + " (copy)" : juce::String ("My preset"), "Name");
    window->addTextEditor ("category", current != nullptr && ! current->isFactory ? current->category : juce::String ("User"), "Category");
    window->addTextEditor ("description", {}, "Description");
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HeaderBar> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window] (int result)
                                                                        {
                                                                            if (safe == nullptr || result != 1)
                                                                                return;
                                                                            juce::String error;
                                                                            const auto id = safe->controller.saveUserPreset (
                                                                                window->getTextEditorContents ("name"), window->getTextEditorContents ("category"),
                                                                                window->getTextEditorContents ("description"), -1, error);
                                                                            if (id.isEmpty())
                                                                                safe->showError ("Could not save the preset", error);
                                                                        }),
                             true);
}

void HeaderBar::renamePreset (const PresetInfo& preset)
{
    if (! preset.isValid() || preset.isFactory)
        return;

    auto* window = new juce::AlertWindow ("Rename preset", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", preset.name, "New name");
    window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HeaderBar> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, preset] (int result)
    {
        if (safe == nullptr || result != 1)
            return;
        const auto newName = window->getTextEditorContents ("name").trim();
        if (newName.isEmpty() || newName == preset.name)
            return;

        // A rename keeps the preset's uuid (docs/11 E52): the strips and the
        // automatic profile rules that play it keep working.
        juce::String error;
        if (safe->controller.renameUserPreset (preset, newName, error).isEmpty())
        {
            safe->showError ("Could not rename the preset", error);
            return;
        }
        safe->refreshPresets();
    }),
                             true);
}

void HeaderBar::deletePreset (const PresetInfo& preset)
{
    if (! preset.isValid() || preset.isFactory)
        return;
    juce::Component::SafePointer<HeaderBar> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete preset",
                                        "Move \"" + preset.name + "\" to the trash? This cannot be undone from Flubsound.", "Delete", "Cancel", this,
                                        juce::ModalCallbackFunction::create ([safe, preset] (int result)
                                                                             {
                                                                                 if (safe == nullptr || result != 1)
                                                                                     return;
                                                                                 juce::String error;
                                                                                 if (! safe->controller.getPresetManager().deleteUserPreset (preset, error))
                                                                                     safe->showError ("Could not delete the preset", error);
                                                                                 safe->refreshPresets();
                                                                             }));
}

void HeaderBar::importPreset()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import a Flubsound preset", juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                       "*.json");
    juce::Component::SafePointer<HeaderBar> safe (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (safe == nullptr || file == juce::File())
                                      return;
                                  juce::String error;
                                  const auto id = safe->controller.getPresetManager().importPresetFile (file, error);
                                  if (id.isEmpty())
                                      safe->showError ("Could not import the preset", error);
                                  else if (! safe->controller.loadPreset (id, -1, error))
                                      safe->showError ("Could not load the preset", error);
                              });
}

void HeaderBar::exportPreset (const PresetInfo& preset)
{
    if (! preset.isValid())
        return;
    const auto target = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile (juce::File::createLegalFileName (preset.name) + ".flubpreset.json");
    fileChooser = std::make_unique<juce::FileChooser> ("Export preset", target, "*.json");
    juce::Component::SafePointer<HeaderBar> safe (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, preset] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (safe == nullptr || file == juce::File())
                                      return;
                                  juce::String error;
                                  if (! safe->controller.getPresetManager().exportPreset (preset, file, error))
                                      safe->showError ("Could not export the preset", error);
                              });
}

void HeaderBar::resetStrip()
{
    // Defaults for the active bank, keeping what is not "sound": the mode of
    // the strip, the (global) latency profile and the master bypass.
    auto& store = controller.getSelectedParams();
    const auto bank = store.getActiveBank();
    const float mode = store.get (Mode), profile = store.get (LatencyProfile), bypass = store.get (BypassAll);
    store.resetToDefaults (bank);
    store.set (Mode, mode);
    store.set (LatencyProfile, profile);
    store.set (BypassAll, bypass);
    controller.getPresetManager().setCurrentPresetId (controller.getSelectedStrip(), {}, &store);
    refresh();
}

void HeaderBar::showBypassMenu()
{
    const bool matched = controller.getSelectedParams().get (LoudnessMatchBypass) >= 0.5f;
    juce::PopupMenu menu;
    menu.addItem (1, "Loudness-matched bypass", true, matched);
    juce::Component::SafePointer<HeaderBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (bypassButton.get()),
                        [safe, matched] (int result)
                        {
                            if (safe == nullptr || result != 1)
                                return;
                            // Application-wide preference: every strip, both banks.
                            for (int s = 0; s < safe->controller.getNumStrips(); ++s)
                                for (const auto bank : { Bank::A, Bank::B })
                                    safe->controller.getParams (s).set (bank, LoudnessMatchBypass, matched ? 0.0f : 1.0f);
                            safe->refresh();
                        });
}

void HeaderBar::showCompareMenu (juce::Component& target)
{
    const bool matched = comparison->isEnabled();
    const auto bank = controller.getActiveBank();
    juce::PopupMenu menu;
    menu.addItem (1, "Loudness-matched A/B and module listen", true, matched);
    menu.addItem (2, bank == Bank::A ? "Copy A to B" : "Copy B to A");
    menu.addSeparator();
    menu.addItem (3, "Blind test (A/B/X)...", onBlindTestRequested != nullptr);
    juce::Component::SafePointer<HeaderBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [safe, matched] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            if (result == 1)
                                safe->setComparisonMatched (! matched);
                            else if (result == 2)
                                safe->controller.copyActiveToOtherBank();
                            else if (result == 3 && safe->onBlindTestRequested != nullptr)
                                safe->onBlindTestRequested();
                        });
}

void HeaderBar::showOverflowMenu()
{
    juce::PopupMenu menu;
    // The readout's lines (the readout itself has no room here).
    menu.addSectionHeader ("Latency " + latencyText + "   " + cpu.caption + " " + cpu.value);
    if (const auto device = describeDeviceSafety (controller.getDeviceSafetyState()); device.isNotEmpty())
        menu.addItem (-1, device, false);
    menu.addSeparator();
    const auto bank = controller.getActiveBank();
    menu.addItem (1, bank == Bank::A ? "Copy A to B" : "Copy B to A");
    menu.addItem (2, "Loudness-matched A/B and module listen", true, comparison->isEnabled());
    menu.addItem (3, "Blind test (A/B/X)...", onBlindTestRequested != nullptr);
    menu.addSeparator();
    menu.addItem (4, "Routing and strips...", onRoutingRequested != nullptr);
    menu.addItem (5, "Settings...", onSettingsRequested != nullptr);
    juce::Component::SafePointer<HeaderBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&overflowButton),
                        [safe] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            switch (result)
                            {
                                case 1: safe->controller.copyActiveToOtherBank(); break;
                                case 2: safe->setComparisonMatched (! safe->comparison->isEnabled()); break;
                                case 3:
                                    if (safe->onBlindTestRequested != nullptr)
                                        safe->onBlindTestRequested();
                                    break;
                                case 4:
                                    if (safe->onRoutingRequested != nullptr)
                                        safe->onRoutingRequested();
                                    break;
                                case 5:
                                    if (safe->onSettingsRequested != nullptr)
                                        safe->onSettingsRequested();
                                    break;
                                default: break;
                            }
                        });
}

// =============================================================================
// Painting / layout
// =============================================================================
void HeaderBar::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (Palette::panel.brighter (0.03f), 0.0f, 0.0f, Palette::panel.darker (0.12f), 0.0f, bounds.getBottom(), false));
    g.fillRect (bounds);
    g.setColour (Palette::border);
    g.fillRect (bounds.withTop (bounds.getBottom() - 1.0f));

    const auto accent = Theme::accent (*this);

    // ---- Logo + wordmark ----
    {
        auto r = logoArea.toFloat();
        const float mark = juce::jmin (30.0f, r.getHeight() - 16.0f);
        auto markArea = r.removeFromLeft (mark).withSizeKeepingCentre (mark, mark);
        g.setGradientFill (juce::ColourGradient (accent.brighter (0.15f), markArea.getX(), markArea.getY(), accent.darker (0.45f), markArea.getRight(),
                                                 markArea.getBottom(), false));
        g.fillRoundedRectangle (markArea, 8.0f);
        drawIcon (g, Icons::logo(), markArea.reduced (mark * 0.16f), Palette::background, 2.4f);
        r.removeFromLeft (10.0f);

        if (! narrow) // narrow windows: the mark alone
        {
            const float size = compact ? 15.0f : 17.0f;
            g.setFont (Theme::font (size, true));
            const float w1 = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), "Flubsound");
            g.setColour (Palette::text);
            g.drawText ("Flubsound", r.removeFromLeft (w1 + 1.0f), juce::Justification::centredLeft, false);
            r.removeFromLeft (4.0f);
            g.setColour (accent);
            g.drawText ("Pro", r, juce::Justification::centredLeft, false);
        }
    }

    // ---- Mode switch track + sliding thumb ----
    {
        const auto track = modeArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (track, track.getHeight() * 0.5f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (track.reduced (0.5f), track.getHeight() * 0.5f, 1.0f);

        const auto a = musicSegment->getBounds().toFloat(), b = gamingSegment->getBounds().toFloat();
        const auto thumb = juce::Rectangle<float> (a.getX() + (b.getX() - a.getX()) * thumbPos, a.getY(), a.getWidth() + (b.getWidth() - a.getWidth()) * thumbPos,
                                                   a.getHeight())
                               .reduced (2.0f);
        const auto thumbColour = Palette::teal.interpolatedWith (Palette::magenta, thumbPos);
        g.setColour (thumbColour.withAlpha (0.18f));
        g.fillRoundedRectangle (thumb.expanded (2.0f), thumb.getHeight() * 0.5f + 2.0f);
        g.setGradientFill (juce::ColourGradient (thumbColour.brighter (0.12f), 0.0f, thumb.getY(), thumbColour.darker (0.2f), 0.0f, thumb.getBottom(), false));
        g.fillRoundedRectangle (thumb, thumb.getHeight() * 0.5f);
    }

    // ---- Segmented containers ----
    auto container = [&g] (juce::Rectangle<int> area)
    {
        if (area.isEmpty())
            return;
        g.setColour (Palette::well);
        g.fillRoundedRectangle (area.toFloat(), Theme::kControlRadius + 1.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), Theme::kControlRadius + 1.0f, 1.0f);
    };
    if (! narrow)
        container (stripArea);
    container (abArea);

    // ---- The comparison lines under A/B and Bypass (docs/11 E37) ----
    {
        const auto line = [&g, this] (const juce::String& text, juce::Rectangle<int> above)
        {
            if (text.isEmpty() || above.isEmpty())
                return;
            const auto area = juce::Rectangle<float> (static_cast<float> (above.getX() - 12), static_cast<float> (above.getBottom()),
                                                      static_cast<float> (above.getWidth() + 24), static_cast<float> (getHeight() - above.getBottom()));
            g.setFont (Theme::font (9.5f, true));
            g.setColour (Palette::muted);
            g.drawText (text, area, juce::Justification::centred, false);
        };
        line (abCaption, abArea);
        line (bypassCaption, bypassButton->getBounds());
    }

    // ---- Latency / CPU readout: caption left (hidden when compact), value right ----
    if (! readoutArea.isEmpty())
    {
        auto r = readoutArea.toFloat();
        auto row1 = r.removeFromTop (r.getHeight() * 0.5f).withTrimmedTop (2.0f);
        auto row2 = r.withTrimmedBottom (2.0f);
        // A sustained overload (OverloadWatchdog) turns the CPU line into the
        // hot status colour, with the caption (or a "!" when compact) saying so.
        const auto hot = Theme::statusColours (*this).hot;
        if (! compact)
        {
            Theme::drawCaption (g, "LATENCY", row1, Palette::faint);
            Theme::drawCaption (g, cpu.caption, row2, cpu.overload ? hot : Palette::faint);
        }
        g.setFont (Theme::numeric (12.5f));
        g.setColour (Palette::text);
        g.drawText (latencyText, row1, juce::Justification::centredRight, false);
        g.setFont (Theme::numeric (12.5f, cpu.overload));
        g.setColour (cpu.overload ? hot : (cpu.warn ? Palette::amber : Palette::muted));
        auto value = cpu.value;
        if (compact && cpu.caption != "DEVICE")
            value = (cpu.overload ? "! " : "CPU ") + value;
        g.drawText (value, row2, juce::Justification::centredRight, false);
    }
}

void HeaderBar::paintOverChildren (juce::Graphics& g)
{
    const auto accent = Theme::accent (*this);

    // Activity dots on the strip selector.
    for (size_t i = 0; i < stripButtons.size() && i < stripActive.size(); ++i)
    {
        if (! stripActive[i] || ! stripButtons[i]->isVisible())
            continue;
        const auto b = stripButtons[i]->getBounds().toFloat();
        g.setColour (accent);
        g.fillEllipse (b.getRight() - 9.0f, b.getY() + 5.0f, 5.0f, 5.0f);
    }

    // "Modified" badge on the preset box's top-right corner (never over the text).
    if (presetModified && presetBox.getSelectedId() > 0)
    {
        const auto b = presetBox.getBounds().toFloat();
        const auto dot = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ b.getRight() - 3.0f, b.getY() + 3.0f });
        g.setColour (Palette::panel);
        g.fillEllipse (dot.expanded (2.0f));
        g.setColour (Palette::amber);
        g.fillEllipse (dot);
    }
}

void HeaderBar::resized()
{
    const int w = getWidth();
    compact = w < 1280;
    narrow = w < kNarrowWidth;
    auto r = getLocalBounds().reduced (narrow ? 12 : 16, 0);
    const int controlH = 32;
    auto centred = [this] (juce::Rectangle<int> area, int h) { return area.withSizeKeepingCentre (area.getWidth(), h).withY ((getHeight() - h) / 2); };
    auto centreY = [&centred] (juce::Rectangle<int> area) { return centred (area, controlH); };

    // Narrow windows (docs/11 E39): the mark without the wordmark, a strip
    // menu, no copy button or readout (both in the overflow menu).
    logoArea = r.removeFromLeft (narrow ? 34 : (compact ? 136 : 160));
    r.removeFromLeft (narrow ? 8 : (compact ? 10 : 14));

    modeArea = centred (r.removeFromLeft (narrow ? 132 : (compact ? 152 : 176)), 34);
    {
        auto m = modeArea.reduced (2, 2);
        musicSegment->setBounds (m.removeFromLeft (m.getWidth() / 2));
        gamingSegment->setBounds (m);
    }
    r.removeFromLeft (narrow ? 8 : (compact ? 10 : 14));

    const int stripW = compact ? 54 : 60;
    for (auto& b : stripButtons)
        b->setVisible (! narrow);
    stripBox.setVisible (narrow);
    if (narrow)
    {
        stripArea = centreY (r.removeFromLeft (100));
        stripBox.setBounds (stripArea);
    }
    else
    {
        stripArea = centreY (r.removeFromLeft (stripW * static_cast<int> (stripButtons.size()) + 4));
        auto s = stripArea.reduced (2, 2);
        for (auto& b : stripButtons)
            b->setBounds (s.removeFromLeft (stripW));
    }

    // Right side, from the right edge.
    settingsButton.setBounds (centred (r.removeFromRight (34), 34));
    r.removeFromRight (4);
    viewButton.setBounds (centred (r.removeFromRight (34), 34));
    overflowButton.setVisible (narrow);
    copyAB.setVisible (! narrow);
    if (narrow)
    {
        r.removeFromRight (4);
        overflowButton.setBounds (centred (r.removeFromRight (34), 34));
        r.removeFromRight (8);
        readoutArea = {};
    }
    else
    {
        r.removeFromRight (compact ? 8 : 10);
        readoutArea = centred (r.removeFromRight ((compact ? 74 : 104) + (wideReadout ? 34 : 0)), 34);
        r.removeFromRight (compact ? 10 : 12);
    }
    bypassButton->setBounds (centreY (r.removeFromRight (compact ? 76 : 84)));
    r.removeFromRight (compact ? 8 : 10);
    if (! narrow)
    {
        copyAB.setBounds (centreY (r.removeFromRight (32)));
        r.removeFromRight (4);
    }
    abArea = centreY (r.removeFromRight (compact ? 60 : 68));
    {
        auto a = abArea.reduced (2, 2);
        abA->setBounds (a.removeFromLeft (a.getWidth() / 2));
        abB->setBounds (a);
    }
    r.removeFromRight (compact ? 10 : 12);

    // Preset browser: right-aligned next to A/B, as wide as fits (prev / next
    // are hidden when narrow). The free space stays between the two clusters.
    r.removeFromLeft (narrow ? 0 : (compact ? 10 : 12));
    presetArea = centreY (r);
    const int menuW = 32 + 4, arrowsW = 28 + 4 + 28 + 4;
    const bool showArrows = presetArea.getWidth() - menuW - arrowsW >= 180;
    const int comboW = juce::jmax (0, juce::jmin (compact ? 250 : 300, presetArea.getWidth() - menuW - (showArrows ? arrowsW : 0)));
    const int groupW = comboW + menuW + (showArrows ? arrowsW : 0);
    auto p = presetArea.withTrimmedLeft (juce::jmax (0, presetArea.getWidth() - groupW));
    presetMenu.setBounds (p.removeFromRight (32));
    p.removeFromRight (4);
    prevPreset.setVisible (showArrows);
    nextPreset.setVisible (showArrows);
    if (showArrows)
    {
        prevPreset.setBounds (p.removeFromLeft (28));
        p.removeFromLeft (4);
        nextPreset.setBounds (p.removeFromRight (28));
        p.removeFromRight (4);
    }
    presetBox.setBounds (p);
}
} // namespace flub::app::ui
