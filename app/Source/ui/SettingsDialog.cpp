#include "SettingsDialog.h"

#include "FlubLookAndFeel.h"
#include "presets/PresetManager.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr int kRowHeight = 30;
constexpr int kCaptionWidth = 190;

void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    Theme::drawCaption (g, title.toUpperCase(), area.toFloat(), Palette::muted);
    g.setColour (Palette::border);
    g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
}

/** Caption + control (+ optional help line) rows, laid out top to bottom. */
struct FormLayout
{
    struct Row
    {
        juce::String caption;
        juce::Component* control = nullptr;
        juce::String help;
        juce::String section; // non-empty: a section title row instead
        int controlWidth = 260;
        juce::Rectangle<int> captionArea, helpArea, sectionArea;
    };

    std::vector<Row> rows;

    void section (const juce::String& title) { rows.push_back ({ {}, nullptr, {}, title, 0, {}, {}, {} }); }
    void row (const juce::String& caption, juce::Component& control, const juce::String& help = {}, int width = 260)
    {
        rows.push_back ({ caption, &control, help, {}, width, {}, {}, {} });
    }

    /** Lays the rows out from the top of `area`; returns the bottom edge used. */
    int layout (juce::Rectangle<int> area)
    {
        int y = area.getY();
        for (auto& r : rows)
        {
            if (r.section.isNotEmpty())
            {
                y += (y > area.getY() ? 14 : 0);
                r.sectionArea = { area.getX(), y, area.getWidth(), 22 };
                y += 30;
                continue;
            }
            r.captionArea = { area.getX(), y, kCaptionWidth, kRowHeight };
            if (r.control != nullptr)
                r.control->setBounds (area.getX() + kCaptionWidth, y + 2, juce::jmin (r.controlWidth, area.getWidth() - kCaptionWidth), kRowHeight - 4);
            y += kRowHeight;
            if (r.help.isNotEmpty())
            {
                const auto lines = juce::jmax (1, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), r.help)
                                                                               / juce::jmax (80.0f, static_cast<float> (area.getWidth() - kCaptionWidth)))));
                r.helpArea = { area.getX() + kCaptionWidth, y + 1, area.getWidth() - kCaptionWidth, lines * 15 + 2 };
                y += lines * 15 + 6;
            }
            y += 4;
        }
        return y;
    }

    void paint (juce::Graphics& g) const
    {
        for (const auto& r : rows)
        {
            if (r.section.isNotEmpty())
            {
                drawSectionTitle (g, r.sectionArea, r.section);
                continue;
            }
            g.setColour (Palette::text.withAlpha (0.88f));
            g.setFont (Theme::font (13.0f));
            g.drawText (r.caption, r.captionArea, juce::Justification::centredLeft, true);
            if (r.help.isNotEmpty())
            {
                g.setColour (Palette::faint.brighter (0.2f));
                g.setFont (Theme::font (11.5f));
                g.drawFittedText (r.help, r.helpArea, juce::Justification::topLeft, 4, 1.0f);
            }
        }
    }
};
} // namespace

// =============================================================================
// Audio page
// =============================================================================
/** Intro, OUTPUT DEVICE PROFILE box and device selector, stacked. The box
    grows with its wrapped text (every guidance message of the profile) and
    the page sets its own height; the dialog shows it in a vertical viewport,
    so long guidance scrolls instead of pushing the selector out of reach. */
class SettingsDialog::AudioPage : public juce::Component
{
public:
    explicit AudioPage (EngineController& c)
        : controller (c)
    {
        // Up to 16 inputs: one 7.1 strip plus three stereo strips via JACK / PipeWire monitors.
        selector = std::make_unique<juce::AudioDeviceSelectorComponent> (controller.getDeviceManager(), 0, 16, 1, 2, false, false, true, false);
        selector->setItemHeight (26);
        addAndMakeVisible (*selector);
        deviceText = describeOutputDevice (controller);
    }

    void refresh()
    {
        const auto text = describeOutputDevice (controller);
        if (text != deviceText)
        {
            deviceText = text;
            resized();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "Audio device");
        introLayout.draw (g, introArea.toFloat());

        // Output device profile (headset families, connection, safety ceiling, guidance).
        auto box = deviceArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);
        auto inner = deviceArea.reduced (kBoxPadX, kBoxPadY);
        Theme::drawCaption (g, "OUTPUT DEVICE PROFILE", inner.removeFromTop (16).toFloat(), Palette::faint);
        inner.removeFromTop (4);
        deviceLayout.draw (g, inner.toFloat());
    }

    void resized() override
    {
        // The text column starts kInset px in, so the selector (x = 0) lines
        // its labels up under it as before.
        const int w = getWidth() - kInset;
        titleArea = { kInset, 0, w, 22 };

        juce::AttributedString intro;
        intro.append ("Flubsound processes the input (a Flubsound / virtual cable device or loopback) and plays the result on the output "
                      "device. Smaller buffers lower the latency; raise them if you hear dropouts.",
                      Theme::font (11.5f), Palette::faint.brighter (0.2f));
        introLayout.createLayout (intro, static_cast<float> (juce::jmax (80, w)));
        introArea = { kInset, 28, w, static_cast<int> (std::ceil (introLayout.getHeight())) };

        deviceLayout = layoutDeviceText (deviceText, w - 2 * kBoxPadX);
        const int boxHeight = kBoxPadY + 16 + 4 + static_cast<int> (std::ceil (deviceLayout.getHeight())) + kBoxPadY + 2;
        deviceArea = { kInset, introArea.getBottom() + 8, w, boxHeight };

        selector->setBounds (0, deviceArea.getBottom() + 10, getWidth(), juce::jmax (1, selector->getHeight()));
        fitHeight();
    }

    // The selector sizes its own height to its controls (device type, channel lists, ...).
    void childBoundsChanged (juce::Component* child) override
    {
        if (child == selector.get())
            fitHeight();
    }

private:
    static constexpr int kInset = 10, kBoxPadX = 12, kBoxPadY = 8;

    void fitHeight()
    {
        const int h = selector->getBottom() + 12;
        if (h != getHeight())
            setSize (getWidth(), h);
    }

    /** First lines: device, profile, connection, ceiling. Every further line
        is one guidance message, drawn as a bulleted paragraph. */
    static juce::TextLayout layoutDeviceText (const juce::String& text, int width)
    {
        const auto font = Theme::font (12.0f);
        const auto lines = juce::StringArray::fromLines (text);
        juce::AttributedString s;
        for (int i = 0; i < lines.size(); ++i)
        {
            const bool guidance = i >= 2;
            if (i > 0)
                s.append (guidance ? "\n\n" : "\n", guidance ? Theme::font (5.0f) : font, Palette::text);
            if (guidance)
                s.append (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2  ")), font, Palette::amber); // bullet
            s.append (lines[i], font, Palette::text.withAlpha (guidance ? 0.78f : 0.88f));
        }
        juce::TextLayout layout;
        layout.createLayout (s, static_cast<float> (juce::jmax (80, width)));
        return layout;
    }

    EngineController& controller;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    juce::String deviceText;
    juce::TextLayout introLayout, deviceLayout;
    juce::Rectangle<int> titleArea, introArea, deviceArea;
};

// =============================================================================
// Processing page
// =============================================================================
class SettingsDialog::ProcessingPage : public juce::Component
{
public:
    ProcessingPage (EngineController& c, std::function<void (MeterPalette)> onPalette, MeterPalette palette)
        : controller (c), onPaletteChanged (std::move (onPalette))
    {
        const auto& profileInfo = layout()[static_cast<size_t> (LatencyProfile)];
        for (size_t i = 0; i < profileInfo.choices.size(); ++i)
            latencyBox.addItem (juce::String (profileInfo.choices[i]), static_cast<int> (i) + 1);
        latencyBox.setTitle ("Latency profile");
        latencyBox.onChange = [this]
        {
            const float v = static_cast<float> (latencyBox.getSelectedId() - 1);
            // Every strip, both banks: strips are padded to the largest
            // latency anyway, and A/B switching must not trigger re-prepares.
            for (int s = 0; s < controller.getNumStrips(); ++s)
            {
                auto& store = controller.getParams (s);
                store.set (Bank::A, LatencyProfile, v);
                store.set (Bank::B, LatencyProfile, v);
            }
        };

        inputModeBox.addItem ("Automatic (virtual cables / loopback only)", 1);
        inputModeBox.addItem ("Always process the device input", 2);
        inputModeBox.addItem ("Off", 3);
        inputModeBox.setTitle ("Device input processing");
        inputModeBox.onChange = [this]
        {
            using Mode = AppSettings::DeviceInputMode;
            const int id = inputModeBox.getSelectedId();
            controller.setDeviceInputMode (id == 2 ? Mode::On : (id == 3 ? Mode::Off : Mode::Automatic));
        };

        for (int s = 0; s < controller.getNumStrips(); ++s)
            inputStripBox.addItem (controller.getStripName (s), s + 1);
        inputStripBox.setTitle ("Strip fed by the device input");
        inputStripBox.onChange = [this] { controller.setDeviceInputStrip (inputStripBox.getSelectedId() - 1); };

        auto& routing = controller.getRouting();
        routingBox.addItem ("Automatic", 1);
        routingBox.addItem ("Endpoint routing", 2);
        routingBox.addItem ("Process capture", 3);
        routingBox.addItem ("Off", 4);
        routingBox.setItemEnabled (2, routing.isEndpointRoutingSupported());
        routingBox.setItemEnabled (3, routing.isCaptureSupported());
        routingBox.setTitle ("Per-app routing method");
        routingBox.onChange = [this]
        {
            using M = AppRouting::Method;
            static constexpr M methods[] = { M::Automatic, M::EndpointRouting, M::ProcessCapture, M::Disabled };
            const int id = juce::jlimit (1, 4, routingBox.getSelectedId());
            controller.getRouting().setMethod (methods[id - 1]);
        };

        paletteBox.addItem ("Standard (green / amber / red)", 1);
        paletteBox.addItem ("Colour-blind safe (blue / yellow / vermillion)", 2);
        paletteBox.setSelectedId (palette == MeterPalette::ColourBlindSafe ? 2 : 1, juce::dontSendNotification);
        paletteBox.setTitle ("Meter colours");
        paletteBox.onChange = [this]
        {
            if (onPaletteChanged != nullptr)
                onPaletteChanged (paletteBox.getSelectedId() == 2 ? MeterPalette::ColourBlindSafe : MeterPalette::Standard);
        };

        for (auto* box : { &latencyBox, &inputModeBox, &inputStripBox, &routingBox, &paletteBox })
            addAndMakeVisible (*box);

        form.section ("Latency");
        form.row ("Latency profile", latencyBox,
                  "Quality adds the spectral noise gate and the highest oversampling. Balanced (~4 ms) is the default; "
                  "Low Latency (~2 ms) suits competitive gaming. Changing it restarts the engine briefly.",
                  220);
        form.section ("Sources");
        form.row ("Device input", inputModeBox, "Automatic only processes inputs that look like a virtual cable or loopback device, never a microphone.",
                  330);
        form.row ("Input feeds strip", inputStripBox, {}, 180);
        form.row ("Per-app routing", routingBox,
                  routing.canEnumerateApps() ? juce::String ("Unsupported methods are greyed out.")
                                             : juce::String ("Per-app routing is not available on this system."),
                  220);
        form.section ("Display");
        form.row ("Meter colours", paletteBox, {}, 330);
        refresh();
    }

    void refresh()
    {
        const int profile = static_cast<int> (std::lround (controller.getSelectedParams().get (LatencyProfile)));
        latencyBox.setSelectedId (profile + 1, juce::dontSendNotification);

        using Mode = AppSettings::DeviceInputMode;
        const auto mode = controller.getSettings().getDeviceInputMode();
        inputModeBox.setSelectedId (mode == Mode::On ? 2 : (mode == Mode::Off ? 3 : 1), juce::dontSendNotification);
        const int strip = controller.findStrip (controller.getSettings().getDeviceInputStripName());
        inputStripBox.setSelectedId (strip >= 0 ? strip + 1 : 1, juce::dontSendNotification);

        using M = AppRouting::Method;
        const auto method = controller.getRouting().getMethod();
        routingBox.setSelectedId (method == M::EndpointRouting ? 2 : (method == M::ProcessCapture ? 3 : (method == M::Disabled ? 4 : 1)),
                                  juce::dontSendNotification);

        const auto li = controller.getLatencyInfo();
        const auto status = controller.getStatus();
        juce::String t;
        t << (status.deviceOpen ? status.deviceName + "  (" + status.deviceTypeName + ")" : juce::String ("No audio device open (offline)")) << "\n";
        t << juce::String (li.sampleRate / 1000.0, 1) << " kHz, " << li.blockSize << " samples per block\n";
        t << "Device in " << juce::String (li.deviceInputMs, 1) << " ms  +  engine " << juce::String (li.engineMs, 1) << " ms  +  device out "
          << juce::String (li.deviceOutputMs, 1) << " ms";
        if (li.captureBufferMs > 0.0)
            t << "  +  app capture " << juce::String (li.captureBufferMs, 1) << " ms";
        t << "  =  " << juce::String (li.totalMs + li.captureBufferMs, 1) << " ms";
        if (t != latencyText)
        {
            latencyText = t;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        form.paint (g);
        drawSectionTitle (g, latencyTitle, "Current latency");
        auto r = latencyArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (Palette::text.withAlpha (0.85f));
        g.setFont (Theme::font (12.0f));
        g.drawFittedText (latencyText, latencyArea.reduced (12, 8), juce::Justification::topLeft, 4, 1.0f);
    }

    void resized() override
    {
        // Flow layout: the live latency block follows the form (anchoring it
        // to the bottom made it collide with the Display rows on short
        // dialogs). SettingsDialog's minimum size keeps all of it visible.
        const auto r = getLocalBounds();
        const int formBottom = form.layout (r);
        latencyTitle = { r.getX(), formBottom + 14, r.getWidth(), 22 };
        latencyArea = { r.getX(), latencyTitle.getBottom() + 6, r.getWidth(), 70 };
    }

private:
    EngineController& controller;
    std::function<void (MeterPalette)> onPaletteChanged;
    juce::ComboBox latencyBox, inputModeBox, inputStripBox, routingBox, paletteBox;
    FormLayout form;
    juce::String latencyText;
    juce::Rectangle<int> latencyArea, latencyTitle;
};

// =============================================================================
// Hotkeys page
// =============================================================================
class SettingsDialog::HotkeysPage : public juce::Component
{
public:
    HotkeysPage (EngineController& c, HotkeyHooks h)
        : controller (c), hooks (std::move (h))
    {
        auto& settings = controller.getSettings();
        Style::set (enabledToggle, "switch");
        enabledToggle.setToggleState (settings.getHotkeysEnabled(), juce::dontSendNotification);
        enabledToggle.onClick = [this]
        {
            controller.getSettings().setHotkeysEnabled (enabledToggle.getToggleState());
            reRegister();
        };
        addAndMakeVisible (enabledToggle);

        for (const auto action : AppSettings::getAllHotkeyActions())
        {
            Row row;
            row.action = action;
            row.name = AppSettings::getHotkeyActionName (action);
            row.editor = std::make_unique<juce::TextEditor>();
            row.editor->setTitle (row.name + " shortcut");
            row.editor->setFont (Theme::font (13.0f));
            row.editor->setJustification (juce::Justification::centredLeft);
            row.editor->setIndents (8, 0);
            row.editor->setTooltip ("Type a chord such as Ctrl+Alt+F, Ctrl+Shift+F5 or None, then press Return");
            auto* editor = row.editor.get();
            row.editor->onReturnKey = [this, action, editor] { commit (action, *editor); };
            row.editor->onFocusLost = [this, action, editor] { commit (action, *editor); };
            row.editor->onEscapeKey = [this, action, editor]
            {
                editor->setText (AppSettings::chordToString (controller.getSettings().getHotkey (action)), false);
                editor->giveAwayKeyboardFocus();
            };
            addAndMakeVisible (*row.editor);

            row.reset = std::make_unique<IconButton> ("Reset " + row.name + " to default", Icons::reset(), IconButton::Style::Framed);
            row.reset->setTooltip ("Default: " + AppSettings::chordToString (AppSettings::getDefaultHotkey (action)));
            row.reset->onClick = [this, action, editor]
            {
                controller.getSettings().setHotkey (action, AppSettings::getDefaultHotkey (action));
                editor->setText (AppSettings::chordToString (AppSettings::getDefaultHotkey (action)), false);
                reRegister();
            };
            addAndMakeVisible (*row.reset);
            rows.push_back (std::move (row));
        }
        refresh();
    }

    void refresh()
    {
        for (auto& row : rows)
            if (! row.editor->hasKeyboardFocus (true))
                row.editor->setText (AppSettings::chordToString (controller.getSettings().getHotkey (row.action)), false);
        updateStatus();
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "System-wide shortcuts");
        g.setFont (Theme::font (13.0f));
        for (const auto& row : rows)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.drawText (row.name, row.captionArea, juce::Justification::centredLeft, true);
        }
        if (status.isNotEmpty())
        {
            g.setColour (statusIsError ? Palette::amber : Palette::faint.brighter (0.2f));
            g.setFont (Theme::font (12.0f));
            g.drawFittedText (status, statusArea, juce::Justification::topLeft, 5, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleArea = r.removeFromTop (22);
        r.removeFromTop (10);
        enabledToggle.setBounds (r.removeFromTop (26).withWidth (300));
        r.removeFromTop (10);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (kRowHeight);
            row.captionArea = line.removeFromLeft (kCaptionWidth);
            row.editor->setBounds (line.removeFromLeft (200).reduced (0, 2));
            line.removeFromLeft (6);
            row.reset->setBounds (line.removeFromLeft (26).reduced (0, 2));
            r.removeFromTop (4);
        }
        r.removeFromTop (10);
        statusArea = r.removeFromTop (80);
    }

private:
    struct Row
    {
        HotkeyAction action {};
        juce::String name;
        std::unique_ptr<juce::TextEditor> editor;
        std::unique_ptr<IconButton> reset;
        juce::Rectangle<int> captionArea;
    };

    void commit (HotkeyAction action, juce::TextEditor& editor)
    {
        auto& settings = controller.getSettings();
        const auto text = editor.getText().trim();
        flub::platform::KeyChord chord;
        if (text.isEmpty() || text.equalsIgnoreCase ("None"))
        {
            settings.setHotkey (action, {});
        }
        else if (AppSettings::chordFromString (text, chord))
        {
            settings.setHotkey (action, chord);
        }
        else
        {
            editor.setText (AppSettings::chordToString (settings.getHotkey (action)), false);
            status = "\"" + text + "\" is not a valid shortcut. Use modifiers + one key, e.g. Ctrl+Alt+F or Ctrl+Shift+F5.";
            statusIsError = true;
            repaint();
            return;
        }
        editor.setText (AppSettings::chordToString (settings.getHotkey (action)), false);
        reRegister();
    }

    void reRegister()
    {
        if (hooks.reRegister != nullptr)
            hooks.reRegister();
        updateStatus();
    }

    void updateStatus()
    {
        const bool supported = hooks.isSupported != nullptr && hooks.isSupported();
        statusIsError = false;
        if (! supported)
        {
            status = "System-wide hotkeys are not available here (no platform support, or a Wayland session without the "
                     "GlobalShortcuts portal). The shortcuts are saved and apply where supported.";
        }
        else
        {
            const auto failures = hooks.getFailures != nullptr ? hooks.getFailures() : juce::StringArray();
            if (failures.isEmpty())
                status = controller.getSettings().getHotkeysEnabled() ? "All shortcuts are registered." : "Shortcuts are switched off.";
            else
            {
                status = "Could not register: " + failures.joinIntoString (", ") + " (probably used by another application).";
                statusIsError = true;
            }
        }
        repaint();
    }

    EngineController& controller;
    HotkeyHooks hooks;
    juce::ToggleButton enabledToggle { "Enable system-wide hotkeys" };
    std::vector<Row> rows;
    juce::String status;
    bool statusIsError = false;
    juce::Rectangle<int> titleArea, statusArea;
};

// =============================================================================
// General page
// =============================================================================
class SettingsDialog::GeneralPage : public juce::Component
{
public:
    explicit GeneralPage (EngineController& c)
        : controller (c)
    {
        auto& settings = controller.getSettings();
        for (auto* t : { &startMinimised, &closeToTray })
        {
            Style::set (*t, "switch");
            addAndMakeVisible (*t);
        }
        startMinimised.setToggleState (settings.getStartMinimised(), juce::dontSendNotification);
        closeToTray.setToggleState (settings.getCloseToTray(), juce::dontSendNotification);
        startMinimised.onClick = [this] { controller.getSettings().setStartMinimised (startMinimised.getToggleState()); };
        closeToTray.onClick = [this] { controller.getSettings().setCloseToTray (closeToTray.getToggleState()); };

        revealSettings.setText ("Show");
        revealPresets.setText ("Show");
        revealSettings.onClick = [this] { controller.getSettings().getFile().revealToUser(); };
        revealPresets.onClick = [this]
        {
            const auto folder = controller.getPresetManager().getUserPresetFolder();
            folder.createDirectory();
            folder.revealToUser();
        };
        addAndMakeVisible (revealSettings);
        addAndMakeVisible (revealPresets);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, startupTitle, "Start-up");
        drawSectionTitle (g, filesTitle, "Files");
        g.setFont (Theme::font (12.5f));
        auto line = [&] (juce::Rectangle<int> area, const juce::String& caption, const juce::String& value)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.drawText (caption, area.removeFromLeft (kCaptionWidth), juce::Justification::centredLeft, true);
            g.setColour (Palette::muted);
            g.drawFittedText (value, area, juce::Justification::centredLeft, 1, 0.7f);
        };
        line (settingsLine, "Settings file", controller.getSettings().getFile().getFullPathName());
        line (presetsLine, "User presets", controller.getPresetManager().getUserPresetFolder().getFullPathName());
        g.setColour (Palette::faint.brighter (0.2f));
        auto* app = juce::JUCEApplicationBase::getInstance();
        g.drawText ("Flubsound Pro " + (app != nullptr ? app->getApplicationVersion() : juce::String()) + "  -  Music & Gaming Edition", versionLine,
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        startupTitle = r.removeFromTop (22);
        r.removeFromTop (10);
        startMinimised.setBounds (r.removeFromTop (26).withWidth (360));
        r.removeFromTop (6);
        closeToTray.setBounds (r.removeFromTop (26).withWidth (360));
        r.removeFromTop (22);
        filesTitle = r.removeFromTop (22);
        r.removeFromTop (10);
        settingsLine = r.removeFromTop (kRowHeight);
        revealSettings.setBounds (settingsLine.removeFromRight (70).reduced (0, 2));
        settingsLine.removeFromRight (8);
        r.removeFromTop (4);
        presetsLine = r.removeFromTop (kRowHeight);
        revealPresets.setBounds (presetsLine.removeFromRight (70).reduced (0, 2));
        presetsLine.removeFromRight (8);
        r.removeFromTop (22);
        versionLine = r.removeFromTop (22);
    }

private:
    EngineController& controller;
    juce::ToggleButton startMinimised { "Start minimised" }, closeToTray { "Close button keeps Flubsound running in the tray" };
    IconButton revealSettings { "Show the settings file", Icons::external(), IconButton::Style::Framed };
    IconButton revealPresets { "Show the user preset folder", Icons::external(), IconButton::Style::Framed };
    juce::Rectangle<int> startupTitle, filesTitle, settingsLine, presetsLine, versionLine;
};

// =============================================================================
// SettingsDialog
// =============================================================================
SettingsDialog::SettingsDialog (EngineController& c, HotkeyHooks hooks, std::function<void (MeterPalette)> onPalette, MeterPalette palette)
    : controller (c)
{
    setTitle ("Flubsound settings");

    static const char* names[] = { "Audio", "Processing", "Hotkeys", "General" };
    for (size_t i = 0; i < navButtons.size(); ++i)
    {
        auto& b = navButtons[i];
        b.setButtonText (names[i]);
        Style::set (b, "tab");
        b.setRadioGroupId (0x5e7, juce::dontSendNotification);
        b.setClickingTogglesState (true);
        b.onClick = [this, i] { showPage (static_cast<Page> (i)); };
        addAndMakeVisible (b);
    }

    audioPage = std::make_unique<AudioPage> (controller);
    processingPage = std::make_unique<ProcessingPage> (controller, std::move (onPalette), palette);
    hotkeysPage = std::make_unique<HotkeysPage> (controller, std::move (hooks));
    generalPage = std::make_unique<GeneralPage> (controller);
    audioView.setViewedComponent (audioPage.get(), false);
    audioView.setScrollBarsShown (true, false);
    audioView.setScrollBarThickness (8);
    addChildComponent (audioView);
    addChildComponent (*processingPage);
    addChildComponent (*hotkeysPage);
    addChildComponent (*generalPage);

    showPage (Page::Audio);
    setSize (780, 600);
    startTimerHz (2);
}

SettingsDialog::~SettingsDialog()
{
    stopTimer();
    audioView.setViewedComponent (nullptr, false);
}

juce::DialogWindow* SettingsDialog::show (EngineController& controller, juce::Component* parent, HotkeyHooks hooks,
                                          std::function<void (MeterPalette)> onPalette, MeterPalette palette, Page page)
{
    auto dialog = std::make_unique<SettingsDialog> (controller, std::move (hooks), std::move (onPalette), palette);
    dialog->showPage (page);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog.release());
    options.dialogTitle = "Flubsound Pro - Settings";
    options.dialogBackgroundColour = Palette::background;
    options.componentToCentreAround = parent;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    auto* window = options.launchAsync();
    if (window != nullptr)
        window->setResizeLimits (kMinWidth, kMinHeight, 1600, 1200);
    return window;
}

juce::String SettingsDialog::describeOutputDevice (EngineController& controller, int maxMessages)
{
    using flub::device::Connection;
    const auto output = controller.getOutputDeviceName();
    if (output.isEmpty())
        return "No output device open.";

    juce::String connection;
    switch (controller.getDeviceConnection())
    {
        case Connection::Analog: connection = "wired"; break;
        case Connection::Usb: connection = "USB / wireless dongle"; break;
        case Connection::Bluetooth: connection = "Bluetooth"; break;
        case Connection::BluetoothHandsFree: connection = "Bluetooth hands-free"; break;
        case Connection::Unknown: break;
    }

    const auto& advice = controller.getDeviceAdvice();
    const auto profile = controller.getDeviceProfileName();
    juce::String s;
    s << output << "  -  " << (profile.isNotEmpty() ? profile : juce::String ("generic device"));
    if (connection.isNotEmpty())
        s << " (" << connection << ")";
    s << "\nSafety ceiling " << juce::String (advice.ceilingDbTp, 1) << " dBTP";
    if (advice.narrowband)
        s << "  -  narrowband (speech) format";
    if (! advice.suggestedPreset.empty())
        s << "  -  suggested preset: " << juce::String::fromUTF8 (advice.suggestedPreset.c_str());
    const auto count = maxMessages < 0 ? advice.messages.size() : std::min (advice.messages.size(), static_cast<size_t> (maxMessages));
    for (size_t i = 0; i < count; ++i)
        s << "\n" << juce::String::fromUTF8 (advice.messages[i].c_str());
    return s;
}

void SettingsDialog::showPage (Page page)
{
    current = page;
    for (size_t i = 0; i < navButtons.size(); ++i)
        navButtons[i].setToggleState (static_cast<int> (i) == static_cast<int> (page), juce::dontSendNotification);
    audioView.setVisible (page == Page::Audio);
    processingPage->setVisible (page == Page::Processing);
    hotkeysPage->setVisible (page == Page::Hotkeys);
    generalPage->setVisible (page == Page::General);
    if (page == Page::Processing)
        processingPage->refresh();
    if (page == Page::Hotkeys)
        hotkeysPage->refresh();
    repaint();
}

void SettingsDialog::timerCallback()
{
    if (current == Page::Processing)
        processingPage->refresh();
    if (current == Page::Audio)
        audioPage->refresh();
}

void SettingsDialog::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    auto nav = navArea.toFloat();
    g.setColour (Palette::panel);
    g.fillRect (nav);
    g.setColour (Palette::border);
    g.fillRect (nav.getRight() - 1.0f, nav.getY(), 1.0f, nav.getHeight());

    auto title = nav.reduced (16.0f, 14.0f).removeFromTop (20.0f);
    Theme::drawCaption (g, "SETTINGS", title, Palette::muted);
}

void SettingsDialog::resized()
{
    auto r = getLocalBounds();
    navArea = r.removeFromLeft (170);
    {
        auto n = navArea.reduced (10, 14);
        n.removeFromTop (30);
        for (auto& b : navButtons)
        {
            b.setBounds (n.removeFromTop (34));
            n.removeFromTop (4);
        }
    }
    pageArea = r.reduced (26, 20);
    // The Audio page extends 10 px to the left (the device selector's labels
    // line up with the text) and its scrollbar sits in the right margin; the
    // page sets its own height for the width it gets.
    const int scrollbar = audioView.getScrollBarThickness() + 6;
    audioView.setBounds (pageArea.withTrimmedLeft (-10).withTrimmedRight (-scrollbar));
    audioPage->setSize (audioView.getWidth() - scrollbar, juce::jmax (1, audioPage->getHeight()));
    processingPage->setBounds (pageArea);
    hotkeysPage->setBounds (pageArea);
    generalPage->setBounds (pageArea);
}
} // namespace flub::app::ui
