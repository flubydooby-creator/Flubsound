#include "SettingsDialog.h"

#include "FlubLookAndFeel.h"
#include "ParameterBinding.h"
#include "platform/PlatformBridge.h"
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

/** Caption + control (+ optional help line) rows, laid out top to bottom. A
    row without a caption uses the full width (e.g. a long switch label). */
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
            const int indent = r.caption.isEmpty() ? 0 : kCaptionWidth;
            r.captionArea = { area.getX(), y, indent, kRowHeight };
            if (r.control != nullptr)
                r.control->setBounds (area.getX() + indent, y + 2, juce::jmin (r.controlWidth, area.getWidth() - indent), kRowHeight - 4);
            y += kRowHeight;
            if (r.help.isNotEmpty())
            {
                const auto lines = juce::jmax (1, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), r.help)
                                                                               / juce::jmax (80.0f, static_cast<float> (area.getWidth() - indent)))));
                r.helpArea = { area.getX() + indent, y + 1, area.getWidth() - indent, lines * 15 + 2 };
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
// Correction page (docs/11 E15)
// =============================================================================
class SettingsDialog::CorrectionPage : public juce::Component
{
public:
    explicit CorrectionPage (EngineController& c)
        : controller (c)
    {
        Style::set (enabledToggle, "switch");
        enabledToggle.onClick = [this] { controller.setDeviceCorrectionEnabled (enabledToggle.getToggleState()); refresh(); };
        compareButton.setClickingTogglesState (true);
        Style::describe (compareButton, "Compare", "Hear the output without the correction's filters, at the same broadband level (the preamp stays)");
        compareButton.onClick = [this] { controller.setDeviceCorrectionCompare (compareButton.getToggleState()); refresh(); };
        Style::describe (importButton, "Import ParametricEQ.txt", "An AutoEQ or Equalizer APO / Peace ParametricEQ.txt for this output device");
        importButton.onClick = [this] { chooseFile(); };
        removeButton.onClick = [this]
        {
            controller.removeDeviceCorrection();
            message = {};
            refresh();
        };
        for (auto* b : { static_cast<juce::Component*> (&importButton), static_cast<juce::Component*> (&enabledToggle),
                         static_cast<juce::Component*> (&compareButton), static_cast<juce::Component*> (&removeButton) })
            addAndMakeVisible (*b);
        refresh();
    }

    void refresh()
    {
        const auto info = controller.getDeviceCorrection();
        enabledToggle.setToggleState (info.enabled, juce::dontSendNotification);
        compareButton.setToggleState (info.comparing, juce::dontSendNotification);
        enabledToggle.setEnabled (info.hasCurve);
        compareButton.setEnabled (info.hasCurve && info.enabled);
        removeButton.setEnabled (info.hasCurve);
        importButton.setEnabled (info.endpoint.isNotEmpty());
        const auto text = (info.endpoint.isNotEmpty() ? "Output: " + info.endpoint : juce::String ("No output device open.")) + "\n"
                          + describeDeviceCorrection (info);
        if (text != statusText)
        {
            statusText = text;
            repaint();
        }
    }

    /** Imports `file` for the current output (the file chooser's result). */
    void importFile (const juce::File& file)
    {
        juce::String error;
        juce::StringArray warnings;
        if (controller.importDeviceCorrection (file, error, &warnings))
            message = warnings.isEmpty() ? "Imported " + file.getFileName() + "."
                                         : "Imported " + file.getFileName() + ", with notes:\n" + warnings.joinIntoString ("\n");
        else
            message = "Not imported: " + error;
        messageIsError = error.isNotEmpty();
        refresh();
        resized();
        repaint();
    }

    const juce::String& getMessage() const noexcept { return message; }
    const juce::String& getStatusText() const noexcept { return statusText; }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "Headphone / speaker correction");
        g.setFont (Theme::font (11.5f));
        g.setColour (Palette::faint.brighter (0.2f));
        g.drawFittedText (kIntro, introArea, juce::Justification::topLeft, 5, 1.0f);
        g.setFont (Theme::font (12.5f));
        g.setColour (Palette::text.withAlpha (0.88f));
        g.drawFittedText (statusText, statusArea, juce::Justification::topLeft, 3, 1.0f);
        if (message.isNotEmpty())
        {
            g.setFont (Theme::font (11.5f));
            g.setColour (messageIsError ? Palette::amber : Palette::muted);
            g.drawFittedText (message, messageArea, juce::Justification::topLeft, 8, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleArea = r.removeFromTop (22);
        r.removeFromTop (8);
        const auto width = static_cast<float> (juce::jmax (80, r.getWidth()));
        const int introLines = juce::jlimit (1, 5, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), kIntro) / width)) + 1);
        introArea = r.removeFromTop (introLines * 15 + 4);
        r.removeFromTop (10);
        statusArea = r.removeFromTop (3 * 17);
        r.removeFromTop (8);
        auto buttons = r.removeFromTop (kRowHeight);
        importButton.setBounds (buttons.removeFromLeft (200).reduced (0, 2));
        buttons.removeFromLeft (10);
        compareButton.setBounds (buttons.removeFromLeft (100).reduced (0, 2));
        buttons.removeFromLeft (10);
        removeButton.setBounds (buttons.removeFromLeft (100).reduced (0, 2));
        r.removeFromTop (8);
        enabledToggle.setBounds (r.removeFromTop (26).withWidth (360));
        r.removeFromTop (12);
        messageArea = r.removeFromTop (8 * 15);
    }

private:
    static constexpr const char* kIntro
        = "A correction curve for the output device: an AutoEQ or Equalizer APO / Peace ParametricEQ.txt. It runs on the final mix, "
          "before the safety limiter, with an automatic preamp so that it cannot clip, and it is remembered for this output only: "
          "presets, A/B and automatic profiles never change it. Flubsound ships no measurement data; use a file whose licence allows it.";

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import a ParametricEQ.txt", juce::File(), "*.txt");
        juce::Component::SafePointer<CorrectionPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->importFile (fc.getResult());
                              });
    }

    EngineController& controller;
    juce::TextButton importButton { "Import ParametricEQ.txt..." }, compareButton { "Compare" }, removeButton { "Remove" };
    juce::ToggleButton enabledToggle { "Correction on for this output" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String statusText, message;
    bool messageIsError = false;
    juce::Rectangle<int> titleArea, introArea, statusArea, messageArea;
};

// =============================================================================
// Processing page
// =============================================================================
class SettingsDialog::ProcessingPage : public juce::Component
{
public:
    ProcessingPage (EngineController& c, std::function<void (MeterPalette)> onPalette, MeterPalette palette)
        : controller (c), onPaletteChanged (std::move (onPalette)), binder ([this] { return &controller.getSelectedParams(); })
    {
        const auto& profileInfo = layout()[static_cast<size_t> (LatencyProfile)];
        for (size_t i = 0; i < profileInfo.choices.size(); ++i)
            latencyBox.addItem (juce::String (profileInfo.choices[i]), static_cast<int> (i) + 1);
        latencyBox.setTitle ("Latency profile");
        latencyBox.onChange = [this]
        {
            // Every strip, both banks (EngineController::setLatencyProfile);
            // a choice by hand also resets the automatic overload response.
            controller.setLatencyProfile (static_cast<LatencyProfileValue> (juce::jlimit (0, 2, latencyBox.getSelectedId() - 1)));
        };

        Style::set (autoReduceToggle, "switch");
        autoReduceToggle.onClick = [this] { controller.setReduceLoadOnOverload (autoReduceToggle.getToggleState()); };
        restoreButton.setTooltip ("Return to the latency profile you chose before the automatic change");
        restoreButton.onClick = [this]
        {
            controller.restoreLatencyProfile();
            refresh();
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

        // Protection (docs/11 E06 / E11).
        protectionBox.addItem ("Off: govern the macro amounts only", 1);
        protectionBox.addItem ("Normal: also the preset's own drive and harmonics", 2);
        protectionBox.addItem ("Strict: as Normal, down to 0", 3);
        protectionBox.setTitle ("Protection strength");
        protectionBox.onChange = [this]
        {
            controller.setProtectionStrength (static_cast<flub::ProtectionStrength> (juce::jlimit (0, 2, protectionBox.getSelectedId() - 1)));
        };
        Style::set (preampToggle, "switch");
        binder.bindToggle (preampToggle, AutoPreampOn);
        preampToggle.setTitle ("Automatic preamp");

        paletteBox.addItem ("Standard (green / amber / red)", 1);
        paletteBox.addItem ("Colour-blind safe (blue / yellow / vermillion)", 2);
        paletteBox.setSelectedId (palette == MeterPalette::ColourBlindSafe ? 2 : 1, juce::dontSendNotification);
        paletteBox.setTitle ("Meter colours");
        paletteBox.onChange = [this]
        {
            if (onPaletteChanged != nullptr)
                onPaletteChanged (paletteBox.getSelectedId() == 2 ? MeterPalette::ColourBlindSafe : MeterPalette::Standard);
        };

        for (auto* box : { &latencyBox, &inputModeBox, &inputStripBox, &routingBox, &protectionBox, &paletteBox })
            addAndMakeVisible (*box);
        addAndMakeVisible (autoReduceToggle);
        addAndMakeVisible (preampToggle);
        addAndMakeVisible (restoreButton);

        form.section ("Latency");
        form.row ("Latency profile", latencyBox,
                  "Quality adds the spectral noise gate and the highest oversampling. Balanced (~4 ms) is the default; "
                  "Low Latency (~2 ms) suits competitive gaming. Changing it restarts the engine briefly.",
                  220);
        form.row ({}, autoReduceToggle,
                  "Off (default): a CPU overload is only reported. On: a lasting overload steps the profile down one level "
                  "(Quality, then Balanced, then Low Latency), at most once every 30 s; it never steps back up by itself.",
                  520);
        form.row ("Automatic change", restoreButton, describeReduction(), 100);
        reductionRow = form.rows.size() - 1;
        form.section ("Sources");
        form.row ("Device input", inputModeBox, "Automatic only processes inputs that look like a virtual cable or loopback device, never a microphone.",
                  330);
        form.row ("Input feeds strip", inputStripBox, {}, 180);
        form.row ("Per-app routing", routingBox,
                  routing.canEnumerateApps() ? juce::String ("Unsupported methods are greyed out.")
                                             : juce::String ("Per-app routing is not available on this system."),
                  220);
        form.section ("Protection");
        form.row ("Protection strength", protectionBox,
                  "The safety governor always scales the Boost and macro amounts back when the limiter works too hard or distortion "
                  "gets audible. Normal also governs the preset's own maximizer drive, saturation drive and bass harmonics; Strict "
                  "lets it go down to 0. Every strip; the Boost panel's governor chip shows what it does.",
                  330);
        form.row ({}, preampToggle, describePreamp(), 520);
        preampRow = form.rows.size() - 1;
        form.section ("Display");
        form.row ("Meter colours", paletteBox, {}, 330);
        refresh();
    }

    void refresh()
    {
        latencyBox.setSelectedId (static_cast<int> (controller.getLatencyProfile()) + 1, juce::dontSendNotification);
        autoReduceToggle.setToggleState (controller.getReduceLoadOnOverload(), juce::dontSendNotification);
        restoreButton.setEnabled (controller.hasReducedLoad());
        if (const auto text = describeReduction(); text != form.rows[reductionRow].help)
        {
            form.rows[reductionRow].help = text;
            resized();
            repaint();
        }

        using Mode = AppSettings::DeviceInputMode;
        const auto mode = controller.getSettings().getDeviceInputMode();
        inputModeBox.setSelectedId (mode == Mode::On ? 2 : (mode == Mode::Off ? 3 : 1), juce::dontSendNotification);
        const int strip = controller.findStrip (controller.getSettings().getDeviceInputStripName());
        inputStripBox.setSelectedId (strip >= 0 ? strip + 1 : 1, juce::dontSendNotification);

        using M = AppRouting::Method;
        const auto method = controller.getRouting().getMethod();
        routingBox.setSelectedId (method == M::EndpointRouting ? 2 : (method == M::ProcessCapture ? 3 : (method == M::Disabled ? 4 : 1)),
                                  juce::dontSendNotification);

        protectionBox.setSelectedId (static_cast<int> (controller.getProtectionStrength()) + 1, juce::dontSendNotification);
        preampToggle.setButtonText ("Automatic preamp on the " + controller.getStripName (controller.getSelectedStrip()) + " strip");
        if (const auto text = describePreamp(); text != form.rows[preampRow].help)
        {
            form.rows[preampRow].help = text;
            resized();
            repaint();
        }

        const auto li = controller.getLatencyInfo();
        const auto status = controller.getStatus();
        juce::String t;
        t << (status.deviceOpen ? status.deviceName + "  (" + status.deviceTypeName + ")" : juce::String ("No audio device open (offline)")) << "\n";
        t << juce::String (li.sampleRate / 1000.0, 1) << " kHz, " << li.blockSize << " samples per block\n";
        t << "Device in " << juce::String (li.deviceInputMs, 1) << " ms  +  engine " << juce::String (li.engineMs, 1) << " ms  +  device out "
          << juce::String (li.deviceOutputMs, 1) << " ms";
        if (li.captureBufferMs > 0.0)
            t << "  +  app capture " << juce::String (li.captureBufferMs, 1) << " ms";
        t << "  =  " << juce::String (li.totalMs + li.captureBufferMs, 1) << " ms\n";
        t << describeCpuLine (status, controller.getOverloadState());

        auto captures = describeCaptureStreams (controller.getCaptureStreams());
        if (captures.isEmpty())
            captures = "No per-app capture streams. Endpoint routing and device inputs do not use one.";
        if (t != latencyText || captures != captureText)
        {
            const bool relayout = captures != captureText; // the capture block wraps its text
            latencyText = t;
            captureText = captures;
            if (relayout)
                resized();
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

        // Per-app capture streams: DriftCompensatedFifo statistics (R1.5).
        drawSectionTitle (g, captureTitle, "Per-app capture streams");
        r = captureArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        captureLayout.draw (g, captureArea.reduced (12, 8).toFloat());
    }

    void resized() override
    {
        // Flow layout: the live latency and capture blocks follow the form
        // (anchoring them to the bottom made them collide with the Display
        // rows on short dialogs). The page sets its own height for its width
        // and scrolls in the dialog when that is taller (one line per
        // capture stream).
        const auto r = getLocalBounds();
        const int formBottom = form.layout (r);
        latencyTitle = { r.getX(), formBottom + 14, r.getWidth(), 22 };
        latencyArea = { r.getX(), latencyTitle.getBottom() + 6, r.getWidth(), 4 * kTextLine + 16 };
        captureTitle = { r.getX(), latencyArea.getBottom() + 14, r.getWidth(), 22 };
        juce::AttributedString text;
        text.setWordWrap (juce::AttributedString::byWord);
        text.setLineSpacing (3.0f);
        text.append (captureText, Theme::font (12.0f), Palette::text.withAlpha (0.85f));
        captureLayout.createLayout (text, static_cast<float> (juce::jmax (80, r.getWidth() - 24)));
        captureArea = { r.getX(), captureTitle.getBottom() + 6, r.getWidth(), static_cast<int> (std::ceil (captureLayout.getHeight())) + 16 };
        if (const int h = captureArea.getBottom() + 4; h != getHeight())
            setSize (getWidth(), h);
    }

private:
    juce::String describeReduction() const
    {
        const auto text = controller.describeLoadReduction();
        return text.isNotEmpty() ? text : juce::String ("None: the latency profile is the one you chose.");
    }

    /** The Automatic Preamp row's help: what it does and, live, what the
        selected strip's chain predicts and takes off (docs/11 E11). */
    juce::String describePreamp()
    {
        auto& chain = controller.getChain (controller.getSelectedStrip());
        juce::String t ("Takes the boost the strip's EQ, bass, clarity and macros add (less a 1 dB allowance) off before processing, "
                        "so hot music does not drive the limiter. Saved with the preset. Predicted boost now: ");
        t << Theme::formatSignedDb (chain.getPredictedBoostDb()) << " dB";
        if (chain.getAutoPreampDb() < -0.05f)
            t << ", preamp " << Theme::formatSignedDb (chain.getAutoPreampDb()) << " dB";
        return t << ".";
    }

    EngineController& controller;
    std::function<void (MeterPalette)> onPaletteChanged;
    juce::ComboBox latencyBox, inputModeBox, inputStripBox, routingBox, protectionBox, paletteBox;
    juce::ToggleButton autoReduceToggle { "Reduce processing load automatically when the CPU overloads" };
    juce::ToggleButton preampToggle { "Automatic preamp" };
    ParameterBinder binder; // after the controls it binds
    size_t preampRow = 0;
    juce::TextButton restoreButton { "Restore" };
    FormLayout form;
    size_t reductionRow = 0; // the "Automatic change" row: its help is the live description
    static constexpr int kTextLine = 15; // line pitch of the 12 px text blocks

    juce::String latencyText, captureText;
    juce::TextLayout captureLayout; // one wrapped paragraph per capture stream
    juce::Rectangle<int> latencyArea, latencyTitle, captureArea, captureTitle;
};

// =============================================================================
// Hotkeys page
// =============================================================================
/** One row per action: name, chord editor, reset button and the action's
    registration status. Results that arrive later (the Wayland portal asks
    the desktop, which may ask the user) are picked up by a 4 Hz poll while
    the page is visible. */
class SettingsDialog::HotkeysPage : public juce::Component, private juce::Timer
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

        // The strip the strip-level hotkeys act on (docs/11 E56), never the
        // strip selected in the window.
        stripBox.setTitle ("Hotkey strip");
        stripBox.setTooltip ("Mode, Boost, preset, Focus, Night and Bypass hotkeys act on this strip, whichever strip is selected "
                             "in the window. While an automatic profile is active, they act on its strip.");
        stripBox.onChange = [this]
        {
            if (const auto name = stripBox.getText(); name.isNotEmpty())
                controller.setHotkeyStripName (name);
        };
        addAndMakeVisible (stripBox);

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

        stripBox.clear (juce::dontSendNotification);
        const auto chosen = controller.getSettings().getHotkeyStripName();
        for (int i = 0; i < controller.getNumStrips(); ++i)
        {
            stripBox.addItem (controller.getStripName (i), i + 1);
            if (controller.getStripName (i).equalsIgnoreCase (chosen))
                stripBox.setSelectedId (i + 1, juce::dontSendNotification);
        }
        if (stripBox.getSelectedId() == 0) // a strip the layout no longer has: the hotkeys use the first one
            stripBox.setSelectedId (controller.getHotkeyStrip() + 1, juce::dontSendNotification);
        updateStatus();
    }

    void visibilityChanged() override
    {
        if (isVisible())
            startTimerHz (4);
        else
            stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "System-wide shortcuts");
        g.setColour (Palette::text.withAlpha (0.88f));
        g.setFont (Theme::font (13.0f));
        g.drawText ("Hotkeys act on", stripCaptionArea, juce::Justification::centredLeft, true);
        for (const auto& row : rows)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.setFont (Theme::font (13.0f));
            g.drawText (row.name, row.captionArea, juce::Justification::centredLeft, true);

            using S = HotkeyManager::Status;
            const auto state = row.state.status;
            g.setColour (state == S::Unavailable || state == S::Declined ? Palette::amber
                         : state == S::Registered                         ? Palette::green
                                                                          : Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (row.stateText, row.stateArea, juce::Justification::centredLeft, 2, 1.0f);
        }
        if (status.isNotEmpty())
        {
            g.setColour (statusIsError ? Palette::amber : Palette::faint.brighter (0.2f));
            g.setFont (Theme::font (12.0f));
            g.drawFittedText (status, statusArea, juce::Justification::topLeft, 8, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleArea = r.removeFromTop (22);
        r.removeFromTop (10);
        enabledToggle.setBounds (r.removeFromTop (26).withWidth (300));
        r.removeFromTop (6);
        {
            auto line = r.removeFromTop (kRowHeight);
            stripCaptionArea = line.removeFromLeft (160);
            stripBox.setBounds (line.removeFromLeft (150).reduced (0, 2));
        }
        r.removeFromTop (6);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (kRowHeight);
            row.captionArea = line.removeFromLeft (160);
            row.editor->setBounds (line.removeFromLeft (150).reduced (0, 2));
            line.removeFromLeft (6);
            row.reset->setBounds (line.removeFromLeft (26).reduced (0, 2));
            line.removeFromLeft (12);
            row.stateArea = line;
            r.removeFromTop (2);
        }
        r.removeFromTop (10);
        statusArea = r;
    }

private:
    struct Row
    {
        HotkeyAction action {};
        juce::String name;
        std::unique_ptr<juce::TextEditor> editor;
        std::unique_ptr<IconButton> reset;
        juce::Rectangle<int> captionArea, stateArea;
        HotkeyManager::ActionStatus state;
        juce::String stateText; // HotkeyManager::describe, "" without the hook
    };

    void timerCallback() override
    {
        // Only a change rewrites the summary, so an invalid-chord message
        // stays until the next change.
        for (const auto& row : rows)
        {
            if (readState (row.action).text != row.stateText)
            {
                updateStatus();
                return;
            }
        }
    }

    struct RowState
    {
        HotkeyManager::ActionStatus state;
        juce::String text;
    };

    RowState readState (HotkeyAction action) const
    {
        if (hooks.getStatus == nullptr)
            return {};
        const auto state = hooks.getStatus (action);
        return { state, HotkeyManager::describe (state) };
    }

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
        using S = HotkeyManager::Status;
        int pending = 0, reassigned = 0, inactive = 0;
        for (auto& row : rows)
        {
            const auto latest = readState (row.action);
            row.state = latest.state;
            row.stateText = latest.text;
            pending += row.state.status == S::Pending ? 1 : 0;
            reassigned += row.state.status == S::Reassigned ? 1 : 0;
            inactive += row.state.status == S::Unavailable || row.state.status == S::Declined ? 1 : 0;
        }

        const bool supported = hooks.isSupported != nullptr && hooks.isSupported();
        statusIsError = false;
        if (! supported)
        {
            status = "System-wide hotkeys are not available here (no platform support, or a Wayland session without the "
                     "GlobalShortcuts portal). The shortcuts are saved and apply where supported.";
        }
        else if (! controller.getSettings().getHotkeysEnabled())
        {
            status = "Shortcuts are switched off.";
        }
        else if (inactive > 0)
        {
            status = "Some shortcuts are not active (see each row): another application may already use the chord, or the desktop "
                     "declined it. Choose another chord, or bind the action in the desktop's keyboard settings.";
            statusIsError = true;
        }
        else if (const auto failures = hooks.getStatus == nullptr && hooks.getFailures != nullptr ? hooks.getFailures() : juce::StringArray();
                 ! failures.isEmpty())
        {
            status = "Could not register: " + failures.joinIntoString ("; ") + ".";
            statusIsError = true;
        }
        else if (pending > 0)
            status = "Waiting for the desktop to confirm the shortcuts; it may ask you in a dialog.";
        else if (reassigned > 0)
            status = "The desktop bound some shortcuts to other keys (shown next to each); change them in its keyboard settings.";
        else
            status = "All shortcuts are registered.";
        repaint();
    }

    EngineController& controller;
    HotkeyHooks hooks;
    juce::ToggleButton enabledToggle { "Enable system-wide hotkeys" };
    juce::ComboBox stripBox;
    juce::Rectangle<int> stripCaptionArea;
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
        : controller (c), autoStart (platform_bridge::createAutoStart())
    {
        auto& settings = controller.getSettings();
        for (auto* t : { &startWithOs, &startMinimised, &closeToTray })
        {
            Style::set (*t, "switch");
            addAndMakeVisible (*t);
        }
        startMinimised.setToggleState (settings.getStartMinimised(), juce::dontSendNotification);
        closeToTray.setToggleState (settings.getCloseToTray(), juce::dontSendNotification);
        startMinimised.onClick = [this] { controller.getSettings().setStartMinimised (startMinimised.getToggleState()); };
        closeToTray.onClick = [this] { controller.getSettings().setCloseToTray (closeToTray.getToggleState()); };

        // Start with the OS: hidden when this OS / build has no support.
        autoStartSupported = autoStart != nullptr && autoStart->isSupported();
        startWithOs.setVisible (autoStartSupported);
        startWithOs.onClick = [this] { applyStartWithOs (startWithOs.getToggleState()); };
        refresh();

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

        // Appearance: both apply at once, app-wide, and are remembered.
        scaleBox.addItem ("Follow system", kFollowSystemId);
        for (const int percent : { 75, 90, 100, 110, 125, 150, 175, 200 })
            scaleBox.addItem (juce::String (percent) + " %", percent);
        const int scale = settings.getUiScalePercent();
        if (scale != AppSettings::kUiScaleFollowSystem && scaleBox.indexOfItemId (scale) < 0)
            scaleBox.addItem (juce::String (scale) + " %", scale); // a value typed into the settings file
        scaleBox.setSelectedId (scale == AppSettings::kUiScaleFollowSystem ? kFollowSystemId : scale, juce::dontSendNotification);
        scaleBox.setTitle ("UI scale");
        scaleBox.setTooltip ("Size of the whole interface. Follow system uses only the operating system's display scaling.");
        scaleBox.onChange = [this]
        {
            const int id = scaleBox.getSelectedId();
            const int percent = id == kFollowSystemId ? AppSettings::kUiScaleFollowSystem : id;
            controller.getSettings().setUiScalePercent (percent);
            Theme::applyUiScale (percent);
        };

        themeBox.addItem ("Standard (dark)", 1);
        themeBox.addItem ("High contrast", 2);
        themeBox.setSelectedId (settings.getHighContrast() ? 2 : 1, juce::dontSendNotification);
        themeBox.setTitle ("Theme");
        themeBox.setTooltip ("High contrast: black surfaces, white text and bright borders (WCAG AA contrast)");
        themeBox.onChange = [this]
        {
            const bool highContrast = themeBox.getSelectedId() == 2;
            controller.getSettings().setHighContrast (highContrast);
            Theme::setTheme (highContrast ? UiTheme::HighContrast : UiTheme::Standard);
        };
        addAndMakeVisible (scaleBox);
        addAndMakeVisible (themeBox);
    }

    /** Shows the OS's ACTUAL start-up entry (the user may have removed it in
        the OS's own settings) and brings the stored setting in line. */
    void refresh()
    {
        if (! autoStartSupported)
            return;
        const bool enabled = autoStart->isEnabled();
        startWithOs.setToggleState (enabled, juce::dontSendNotification);
        controller.getSettings().setStartWithOs (enabled);
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, startupTitle, "Start-up");
        drawSectionTitle (g, appearanceTitle, "Appearance");
        drawSectionTitle (g, filesTitle, "Files");
        if (autoStartError.isNotEmpty())
        {
            g.setColour (Palette::amber);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (autoStartError, autoStartErrorArea, juce::Justification::topLeft, 3, 1.0f);
        }
        g.setFont (Theme::font (12.5f));
        auto line = [&] (juce::Rectangle<int> area, const juce::String& caption, const juce::String& value)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.drawText (caption, area.removeFromLeft (kCaptionWidth), juce::Justification::centredLeft, true);
            g.setColour (Palette::muted);
            g.drawFittedText (value, area, juce::Justification::centredLeft, 1, 0.7f);
        };
        g.setColour (Palette::text.withAlpha (0.88f));
        g.drawText ("UI scale", scaleLine, juce::Justification::centredLeft, true);
        g.drawText ("Theme", themeLine, juce::Justification::centredLeft, true);
        line (settingsLine, "Settings file", controller.getSettings().getFile().getFullPathName());
        line (presetsLine, "User presets", controller.getPresetManager().getUserPresetFolder().getFullPathName());
        g.setColour (Palette::faint.brighter (0.2f));
        auto* app = juce::JUCEApplicationBase::getInstance();
        g.drawText ("Flubsound Pro " + (app != nullptr ? app->getApplicationVersion() : juce::String()) + "  -  Music & Gaming Edition", versionLine,
                    juce::Justification::centredLeft, true);
        g.drawText ("By Flubes and Claude (co-authors)", creditsLine, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        startupTitle = r.removeFromTop (22);
        r.removeFromTop (10);
        if (autoStartSupported)
        {
            startWithOs.setBounds (r.removeFromTop (26).withWidth (360));
            if (autoStartError.isNotEmpty())
            {
                const auto width = static_cast<float> (juce::jmax (80, r.getWidth() - 4));
                const auto lines = juce::jlimit (1, 3, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), autoStartError) / width)));
                autoStartErrorArea = r.removeFromTop (lines * 15 + 4).withTrimmedLeft (4).withTrimmedTop (2);
            }
            r.removeFromTop (6);
        }
        startMinimised.setBounds (r.removeFromTop (26).withWidth (360));
        r.removeFromTop (6);
        closeToTray.setBounds (r.removeFromTop (26).withWidth (360));
        r.removeFromTop (22);
        appearanceTitle = r.removeFromTop (22);
        r.removeFromTop (10);
        scaleLine = r.removeFromTop (kRowHeight);
        scaleBox.setBounds (scaleLine.withTrimmedLeft (kCaptionWidth).withWidth (200).reduced (0, 2));
        r.removeFromTop (4);
        themeLine = r.removeFromTop (kRowHeight);
        themeBox.setBounds (themeLine.withTrimmedLeft (kCaptionWidth).withWidth (200).reduced (0, 2));
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
        creditsLine = r.removeFromTop (18);
    }

private:
    void applyStartWithOs (bool shouldStart)
    {
        // Empty path: the platform layer picks the running executable (on
        // Linux the AppImage itself rather than its temporary mount).
        std::string error;
        const bool ok = autoStart->setEnabled (shouldStart, {}, error);
        const bool enabled = autoStart->isEnabled();
        if (ok && enabled != shouldStart)
            error = shouldStart ? "The start-up entry was written, but the system does not report it as active."
                                : "The start-up entry could not be removed.";

        autoStartError = error.empty() ? juce::String() : juce::String::fromUTF8 (error.c_str());
        startWithOs.setToggleState (enabled, juce::dontSendNotification);
        controller.getSettings().setStartWithOs (enabled);
        resized();
        repaint();
    }

    EngineController& controller;
    std::unique_ptr<flub::platform::AutoStart> autoStart;
    bool autoStartSupported = false;
    juce::ToggleButton startWithOs { "Start Flubsound Pro when I sign in" };
    juce::ToggleButton startMinimised { "Start minimised" }, closeToTray { "Close button keeps Flubsound running in the tray" };
    juce::String autoStartError;
    juce::Rectangle<int> autoStartErrorArea;
    IconButton revealSettings { "Show the settings file", Icons::external(), IconButton::Style::Framed };
    IconButton revealPresets { "Show the user preset folder", Icons::external(), IconButton::Style::Framed };
    static constexpr int kFollowSystemId = 1; // other UI scale items use their percentage as id
    juce::ComboBox scaleBox, themeBox;
    juce::Rectangle<int> startupTitle, appearanceTitle, filesTitle, settingsLine, presetsLine, versionLine, creditsLine, scaleLine, themeLine;
};

// =============================================================================
// SettingsDialog
// =============================================================================
SettingsDialog::SettingsDialog (EngineController& c, HotkeyHooks hooks, std::function<void (MeterPalette)> onPalette, MeterPalette palette)
    : controller (c)
{
    setTitle ("Flubsound settings");

    static const char* names[] = { "Audio", "Correction", "Processing", "Hotkeys", "General" };
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
    correctionPage = std::make_unique<CorrectionPage> (controller);
    processingPage = std::make_unique<ProcessingPage> (controller, std::move (onPalette), palette);
    hotkeysPage = std::make_unique<HotkeysPage> (controller, std::move (hooks));
    generalPage = std::make_unique<GeneralPage> (controller);
    audioView.setViewedComponent (audioPage.get(), false);
    processingView.setViewedComponent (processingPage.get(), false);
    for (auto* view : { &audioView, &processingView })
    {
        view->setScrollBarsShown (true, false);
        view->setScrollBarThickness (8);
        addChildComponent (*view);
    }
    addChildComponent (*correctionPage);
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
    processingView.setViewedComponent (nullptr, false);
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
    {
        window->setResizeLimits (kMinWidth, kMinHeight, 1600, 1200);
        Theme::setMinimumWindowSize (*window, { kMinWidth, kMinHeight }); // never larger than the screen at a large UI scale
    }
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

juce::String SettingsDialog::describeCaptureStreams (const std::vector<EngineController::CaptureStream>& streams)
{
    const auto count = [] (uint64_t n, const char* singular, const char* plural)
    { return juce::String (static_cast<juce::int64> (n)) + " " + (n == 1 ? singular : plural); };

    juce::StringArray lines;
    for (const auto& stream : streams)
    {
        const auto& st = stream.info.stats;
        const int ppm = juce::roundToInt (st.correctionPpm);
        juce::String line;
        line << (stream.appName.isNotEmpty() ? stream.appName : "Process " + juce::String (stream.info.processId));
        if (stream.stripName.isNotEmpty())
            line << " (" << stream.stripName << ")";
        line << ": ";
        if (! stream.info.running)
            line << "stopped  -  ";
        else if (! st.streaming)
            line << "priming  -  ";
        line << "fill " << juce::String (st.fillMs, 1) << " / " << juce::String (st.targetMs, 1) << " ms, drift " << (ppm > 0 ? "+" : "") << ppm
             << " ppm  -  " << count (st.underruns, "underrun", "underruns") << ", " << count (st.overflows, "overflow", "overflows");
        if (st.droppedFrames > 0)
            line << ", " << count (st.droppedFrames, "frame", "frames") << " dropped";
        lines.add (line);
    }
    return lines.joinIntoString ("\n");
}

juce::String SettingsDialog::describeCpuLine (const EngineStatus& status, const OverloadWatchdog::State& overload)
{
    if (! status.deviceOpen)
        return "CPU  -  no audio device open";
    juce::String t;
    t << "CPU " << juce::roundToInt (status.cpuLoad * 100.0) << " %";
    if (status.xruns >= 0)
        t << "  -  " << status.xruns << (status.xruns == 1 ? " xrun" : " xruns");
    t << "  -  ";
    if (overload.overloaded)
        t << "OVERLOAD now (peak " << juce::roundToInt (overload.peakLoad * 100.0) << " %)";
    else
        t << static_cast<juce::int64> (overload.episodes) << (overload.episodes == 1 ? " overload" : " overloads") << " this session";
    return t;
}

juce::String SettingsDialog::describeDeviceCorrection (const EngineController::DeviceCorrectionInfo& info)
{
    if (info.endpoint.isEmpty())
        return "Open an output device to import a correction for it.";
    if (! info.hasCurve)
        return "No correction for this output.";
    juce::String s;
    s << (info.name.isNotEmpty() ? info.name : juce::String ("Imported curve")) << "  -  " << info.numFilters
      << (info.numFilters == 1 ? " filter" : " filters");
    if (! info.enabled)
        return s << "  -  off";
    const auto hz = info.maxBoostHz >= 1000.0 ? juce::String (info.maxBoostHz / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (info.maxBoostHz)) + " Hz";
    s << "  -  preamp " << juce::String (info.preampDb, 1) << " dB (max boost " << (info.maxBoostDb >= 0.0 ? "+" : "")
      << juce::String (info.maxBoostDb, 1) << " dB at " << hz << ")";
    if (info.comparing)
        s << "  -  comparing (filters off)";
    return s;
}

void SettingsDialog::showPage (Page page)
{
    current = page;
    for (size_t i = 0; i < navButtons.size(); ++i)
        navButtons[i].setToggleState (static_cast<int> (i) == static_cast<int> (page), juce::dontSendNotification);
    audioView.setVisible (page == Page::Audio);
    correctionPage->setVisible (page == Page::Correction);
    processingView.setVisible (page == Page::Processing);
    hotkeysPage->setVisible (page == Page::Hotkeys);
    generalPage->setVisible (page == Page::General);
    if (page == Page::Processing)
        processingPage->refresh();
    if (page == Page::Correction)
        correctionPage->refresh();
    if (page == Page::Hotkeys)
        hotkeysPage->refresh();
    if (page == Page::General)
        generalPage->refresh();
    repaint();
}

void SettingsDialog::timerCallback()
{
    if (current == Page::Processing)
        processingPage->refresh();
    if (current == Page::Audio)
        audioPage->refresh();
    if (current == Page::Correction)
        correctionPage->refresh();
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
    // The Processing page scrolls the same way (its scrollbar in the right
    // margin, so its content keeps the page width).
    processingView.setBounds (pageArea.withTrimmedRight (-scrollbar));
    processingPage->setSize (pageArea.getWidth(), juce::jmax (1, processingPage->getHeight()));
    correctionPage->setBounds (pageArea);
    hotkeysPage->setBounds (pageArea);
    generalPage->setBounds (pageArea);
}
} // namespace flub::app::ui
