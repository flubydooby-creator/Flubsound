#include "SettingsDialog.h"

#include "FlubLookAndFeel.h"
#include "HearingPage.h"
#include "HotkeyCapture.h"
#include "LatencyPanel.h"
#include "ParameterBinding.h"
#include "diagnostics/CrashHandler.h"
#include "diagnostics/DiagnosticLog.h"
#include "diagnostics/DiagnosticsBundle.h"
#include "diagnostics/UpdateCheck.h"
#include "platform/PlatformBridge.h"
#include "presets/PresetManager.h"

#include <algorithm>
#include <cmath>
#include <optional>

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
        int height = kRowHeight; // the control's row (a fixed height: live text in it never moves the rows below)
    };

    std::vector<Row> rows;

    void section (const juce::String& title) { rows.push_back ({ {}, nullptr, {}, title, 0, {}, {}, {}, kRowHeight }); }
    void row (const juce::String& caption, juce::Component& control, const juce::String& help = {}, int width = 260, int height = kRowHeight)
    {
        rows.push_back ({ caption, &control, help, {}, width, {}, {}, {}, height });
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
                r.control->setBounds (area.getX() + indent, y + 2, juce::jmin (r.controlWidth, area.getWidth() - indent), r.height - 4);
            y += r.height;
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
class SettingsDialog::AudioPage : public juce::Component, private juce::ComboBox::Listener
{
public:
    explicit AudioPage (EngineController& c)
        : controller (c), latencyPanel (c)
    {
        // Up to 16 inputs: one 7.1 strip plus three stereo strips via JACK / PipeWire monitors.
        selector = std::make_unique<juce::AudioDeviceSelectorComponent> (controller.getDeviceManager(), 0, 16, 1, 2, false, false, true, false);
        selector->setItemHeight (26);
        addAndMakeVisible (*selector);
        addAndMakeVisible (latencyPanel); // docs/11 E42c / E42d, under the selector
        deviceText = describeOutputDevice (controller);

        // Feedback-loop guard override (docs/11 E51).
        allowButton.setTooltip ("Let this input / output pair play although it looks like a feedback loop, e.g. a cable you monitor on purpose");
        allowButton.onClick = [this]
        {
            const auto pair = loopbackPairToAllow (controller);
            controller.setLoopbackPairAllowed (pair.input, pair.output, true);
            refresh();
        };
        addAndMakeVisible (allowButton);

        // docs/11 E16: the headset's own enhancement, per output endpoint (the
        // device banner asks the same once for a headset that has one).
        Style::set (enhancementToggle, "switch");
        enhancementToggle.setTooltip ("On while the headset (or its app) applies Superhuman Hearing, its own EQ or surround: Flubsound "
                                      "then caps Footsteps and Detail at 30 % and turns its virtual surround off on this output, so "
                                      "the two do not stack. Stored for this output device.");
        enhancementToggle.onClick = [this]
        {
            controller.setOnboardEnhancement (enhancementToggle.getToggleState());
            refresh();
        };
        addAndMakeVisible (enhancementToggle);

        // docs/11 E51: follow the system default output instead of the chosen one.
        Style::set (followDefaultToggle, "switch");
        followDefaultToggle.setTooltip ("Play on whatever output the system uses as its default, and switch when it changes (never onto "
                                        "a virtual cable or a feedback loop). Off: the output chosen below. Choosing another output below turns "
                                        "this off.");
        followDefaultToggle.onClick = [this]
        {
            controller.setFollowSystemDefaultOutput (followDefaultToggle.getToggleState());
            refresh();
        };
        addAndMakeVisible (followDefaultToggle);
        refresh();
    }

    ~AudioPage() override
    {
        if (auto* box = bufferList.getComponent())
            box->removeListener (this);
    }

    LatencyPanel& getLatencyPanel() noexcept { return latencyPanel; }

    void refresh()
    {
        hookBufferList();
        const int panelHeight = latencyPanel.getHeight();
        latencyPanel.refresh();
        if (latencyPanel.getHeightForWidth (getWidth()) != panelHeight)
            placeLatencyPanel();

        const auto text = describeOutputDevice (controller);
        const auto guard = describeLoopbackGuard (controller);
        const auto pairs = controller.getAllowedLoopbackPairs();
        allowButton.setEnabled (loopbackPairToAllow (controller).input.isNotEmpty());
        const auto onboard = controller.getOnboardEnhancement();
        enhancementToggle.setEnabled (onboard.endpoint.isNotEmpty());
        enhancementToggle.setToggleState (onboard.on, juce::dontSendNotification);
        followDefaultToggle.setToggleState (controller.getFollowSystemDefaultOutput(), juce::dontSendNotification);
        const auto note = describeDeviceTypeNote (controller.getDeviceManager().getCurrentAudioDeviceType());
        if (text != deviceText || guard != guardText || pairs != shownPairs || note != typeNote)
        {
            deviceText = text;
            guardText = guard;
            typeNote = note;
            if (pairs != shownPairs)
                rebuildPairRows (pairs);
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

        // Feedback-loop guard: its state, "Allow this pair" and the allowed pairs.
        box = guardArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);
        Theme::drawCaption (g, "FEEDBACK-LOOP GUARD", guardArea.reduced (kBoxPadX, kBoxPadY).removeFromTop (16).toFloat(), Palette::faint);
        guardLayout.draw (g, guardTextArea.toFloat());
        g.setFont (Theme::font (12.0f));
        g.setColour (Palette::text.withAlpha (0.85f));
        for (size_t i = 0; i < shownPairs.size() && i < pairRows.size(); ++i)
            g.drawFittedText ("Allowed: input \"" + shownPairs[i].input + "\", output \"" + shownPairs[i].output + "\"", pairRows[i].textArea,
                              juce::Justification::centredLeft, 1, 0.9f);

        // R1.2: what the current device type means for other apps.
        if (! noteArea.isEmpty())
        {
            box = noteArea.toFloat();
            g.setColour (Palette::well);
            g.fillRoundedRectangle (box, 6.0f);
            g.setColour (Palette::amber.withAlpha (0.55f));
            g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);
            auto noteInner = noteArea.reduced (kBoxPadX, kBoxPadY);
            Theme::drawCaption (g, "DEVICE TYPE", noteInner.removeFromTop (16).toFloat(), Palette::amber);
            noteInner.removeFromTop (4);
            noteLayout.draw (g, noteInner.toFloat());
        }
    }

    juce::String getTypeNote() const { return typeNote; }

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
        const int textH = static_cast<int> (std::ceil (deviceLayout.getHeight()));
        const int boxHeight = kBoxPadY + 16 + 4 + textH + 6 + kToggleH + kBoxPadY + 2;
        deviceArea = { kInset, introArea.getBottom() + 8, w, boxHeight };
        enhancementToggle.setBounds (kInset + kBoxPadX, deviceArea.getY() + kBoxPadY + 20 + textH + 6, w - 2 * kBoxPadX, kToggleH);

        // Guard box: caption, wrapped text, [Allow this pair], one row per allowed pair.
        {
            juce::AttributedString t;
            t.setWordWrap (juce::AttributedString::byWord);
            t.append (guardText, Theme::font (12.0f), Palette::text.withAlpha (0.85f));
            const int textW = w - 2 * kBoxPadX;
            guardLayout.createLayout (t, static_cast<float> (juce::jmax (80, textW)));
            int y = deviceArea.getBottom() + 8 + kBoxPadY + 20;
            guardTextArea = { kInset + kBoxPadX, y, textW, static_cast<int> (std::ceil (guardLayout.getHeight())) };
            y = guardTextArea.getBottom() + 6;
            allowButton.setBounds (kInset + kBoxPadX, y, 150, 26);
            y += 30;
            for (auto& row : pairRows)
            {
                row.remove->setBounds (kInset + w - kBoxPadX - 80, y, 80, 24);
                row.textArea = { kInset + kBoxPadX, y, juce::jmax (40, textW - 88), 24 };
                y += 28;
            }
            guardArea = { kInset, deviceArea.getBottom() + 8, w, y - deviceArea.getBottom() - 8 + kBoxPadY - 4 };
        }

        followDefaultToggle.setBounds (kInset, guardArea.getBottom() + 10, w, kToggleH);
        int selectorTop = followDefaultToggle.getBottom() + 6;

        // The device type's note (R1.2), right above the selector it is about.
        noteArea = {};
        if (typeNote.isNotEmpty())
        {
            juce::AttributedString t;
            t.setWordWrap (juce::AttributedString::byWord);
            t.append (typeNote, Theme::font (12.0f), Palette::text.withAlpha (0.85f));
            noteLayout.createLayout (t, static_cast<float> (juce::jmax (80, w - 2 * kBoxPadX)));
            const int noteH = kBoxPadY + 16 + 4 + static_cast<int> (std::ceil (noteLayout.getHeight())) + kBoxPadY;
            noteArea = { kInset, selectorTop + 2, w, noteH };
            selectorTop = noteArea.getBottom() + 6;
        }
        selector->setBounds (0, selectorTop, getWidth(), juce::jmax (1, selector->getHeight()));
        placeLatencyPanel();
    }

    // The selector sizes its own height to its controls (device type, channel lists, ...).
    void childBoundsChanged (juce::Component* child) override
    {
        if (child == selector.get())
            placeLatencyPanel();
    }

private:
    static constexpr int kInset = 10, kBoxPadX = 12, kBoxPadY = 8, kToggleH = 26;

    struct PairRow
    {
        std::unique_ptr<juce::TextButton> remove;
        juce::Rectangle<int> textArea;
    };

    void rebuildPairRows (const std::vector<AppSettings::LoopbackPair>& pairs)
    {
        shownPairs = pairs;
        pairRows.clear();
        for (const auto& pair : pairs)
        {
            PairRow row;
            row.remove = std::make_unique<juce::TextButton> ("Remove");
            row.remove->setTooltip ("Mute this pair again when it would feed the output back into the input");
            row.remove->onClick = [this, pair]
            {
                controller.setLoopbackPairAllowed (pair.input, pair.output, false);
                refresh();
            };
            addAndMakeVisible (*row.remove);
            pairRows.push_back (std::move (row));
        }
    }

    void placeLatencyPanel()
    {
        latencyPanel.setBounds (0, selector->getBottom() + 10, getWidth(), latencyPanel.getHeightForWidth (getWidth()));
        fitHeight();
    }

    void fitHeight()
    {
        const int h = latencyPanel.getBottom() + 12;
        if (h != getHeight())
            setSize (getWidth(), h);
    }

    /** docs/11 E42c: a size picked in the selector's "Audio buffer size"
        list is the user's choice, so Automatic turns off. The list lives
        inside JUCE's selector (re-created with the device type), found by its
        attached label. */
    void hookBufferList()
    {
        juce::ComboBox* found = nullptr;
        std::function<void (juce::Component&)> search = [&] (juce::Component& parent)
        {
            for (auto* child : parent.getChildren())
            {
                if (found != nullptr)
                    return;
                if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText().startsWith ("Audio buffer size"))
                    found = dynamic_cast<juce::ComboBox*> (label->getAttachedComponent());
                search (*child);
            }
        };
        search (*selector);
        if (found == bufferList.getComponent())
            return;
        if (auto* old = bufferList.getComponent())
            old->removeListener (this);
        bufferList = found;
        if (found != nullptr)
            found->addListener (this);
    }

    void comboBoxChanged (juce::ComboBox* box) override
    {
        // Listeners hear the pick before JUCE applies it (onChange): the
        // device still runs at the old size here.
        if (box != bufferList.getComponent() || box->getSelectedId() <= 0 || ! controller.getAutomaticBufferSize())
            return;
        if (box->getSelectedId() != controller.getBufferInfo().current)
            controller.setAutomaticBufferSize (false);
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
    LatencyPanel latencyPanel;                         // docs/11 E42c / E42d
    juce::Component::SafePointer<juce::ComboBox> bufferList; // the selector's buffer size list (hookBufferList)
    juce::String deviceText, guardText, typeNote;
    juce::TextLayout introLayout, deviceLayout, guardLayout, noteLayout;
    juce::Rectangle<int> titleArea, introArea, deviceArea, guardArea, guardTextArea, noteArea;
    juce::TextButton allowButton { "Allow this pair" };
    juce::ToggleButton enhancementToggle { "Headset enhancement (Superhuman Hearing / on-board EQ) is ON" };
    juce::ToggleButton followDefaultToggle { "Follow the system default output" };
    std::vector<AppSettings::LoopbackPair> shownPairs;
    std::vector<PairRow> pairRows;
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
namespace
{
/** The loudness contour's target lift (docs/11 E32), read-only: 20 Hz ..
    20 kHz on a log axis, -6 .. +18 dB, the 29 ISO 226 points joined. */
class ContourCurveView : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kMinDb = -6.0f, kMaxDb = 18.0f;

    ContourCurveView()
    {
        setTitle ("Loudness contour curve");
        setTooltip ("What the loudness contour adds now, relative to 1 kHz (ISO 226 equal loudness). Read-only: it follows the "
                    "volume, the reference and the preset's Listening Level.");
        setDescription (SettingsDialog::describeContourCurve (curve, 0.0f));
    }

    void setCurve (const SettingsDialog::ContourCurve& next)
    {
        if (next.on == curve.on && next.liftDb == curve.liftDb)
            return;
        curve = next;
        setDescription (SettingsDialog::describeContourCurve (curve, 0.0f));
        repaint();
    }
    const SettingsDialog::ContourCurve& getCurve() const noexcept { return curve; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
        const auto plot = bounds.reduced (30.0f, 10.0f).withTrimmedBottom (10.0f);
        const auto xOf = [&plot] (double hz)
        { return plot.getX() + plot.getWidth() * static_cast<float> (std::log10 (hz / 20.0) / std::log10 (1000.0)); };
        const auto yOf = [&plot] (float db)
        { return plot.getBottom() - plot.getHeight() * (juce::jlimit (kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb); };

        g.setFont (Theme::font (10.0f));
        for (const float db : { 0.0f, 6.0f, 12.0f, 18.0f })
        {
            g.setColour (db == 0.0f ? Palette::muted.withAlpha (0.6f) : Palette::border);
            g.fillRect (plot.getX(), yOf (db) - 0.5f, plot.getWidth(), 1.0f);
            g.setColour (Palette::faint);
            g.drawText (Theme::formatSignedDb (db, 0), juce::Rectangle<float> (bounds.getX() + 2.0f, yOf (db) - 7.0f, 26.0f, 14.0f),
                        juce::Justification::centredRight, false);
        }
        for (const auto& [hz, text] : { std::pair { 50.0, "50" }, std::pair { 100.0, "100" }, std::pair { 1000.0, "1k" },
                                        std::pair { 10000.0, "10k" } })
        {
            const float x = xOf (hz);
            g.setColour (Palette::border);
            g.fillRect (x - 0.5f, plot.getY(), 1.0f, plot.getHeight());
            g.setColour (Palette::faint);
            g.drawText (text, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred, false);
        }

        juce::Path path;
        for (int i = 0; i < flub::iso226::kNumFrequencies; ++i)
        {
            const auto p = juce::Point<float> (xOf (flub::iso226::kFrequencies[static_cast<size_t> (i)]),
                                               yOf (curve.liftDb[static_cast<size_t> (i)]));
            if (i == 0)
                path.startNewSubPath (p);
            else
                path.lineTo (p);
        }
        g.setColour (curve.on ? Theme::accent (*this) : Palette::faint);
        g.strokePath (path, juce::PathStrokeType (curve.on ? 2.0f : 1.2f, juce::PathStrokeType::curved));
        if (! curve.on)
        {
            g.setColour (Palette::faint);
            g.setFont (Theme::font (11.5f));
            g.drawText ("Contour off", plot.withTrimmedBottom (plot.getHeight() * 0.35f), juce::Justification::centred, false);
        }
    }

private:
    SettingsDialog::ContourCurve curve;
};
} // namespace

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

        // (The device input and per-app routing moved to the Routing page.)

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
        // Its hot-programme term (docs/11 E11, auto.preampHot): acts only
        // with the automatic preamp on, so the switch is dimmed without it.
        Style::set (preampHotToggle, "switch");
        binder.bindToggle (preampHotToggle, AutoPreampHot);
        preampHotToggle.setTitle ("Automatic preamp on hot programme");
        // Smart macros (docs/11 E34): a host setting per strip, not a parameter.
        Style::set (smartToggle, "switch");
        smartToggle.setTitle ("Smart macros");
        smartToggle.onClick = [this] { controller.setSmartMacros (smartToggle.getToggleState()); };

        // Listening level (docs/11 E32): the contour is the preset's
        // (contour.on), following the system volume is the app's.
        Style::set (contourToggle, "switch");
        binder.bindToggle (contourToggle, ContourOn);
        contourToggle.setTitle ("Loudness contour");
        Style::set (followToggle, "switch");
        followToggle.setTitle ("Follow the system volume");
        followToggle.onClick = [this]
        {
            controller.setContourFollowsVolume (followToggle.getToggleState());
            refresh();
        };
        referenceSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        referenceSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
        referenceSlider.setRange (kMinReferenceDb, kMaxReferenceDb, 0.5);
        referenceSlider.setTextValueSuffix (" dB");
        referenceSlider.setTitle ("Reference volume");
        referenceSlider.onValueChange = [this]
        {
            if (! refreshing)
                controller.setContourReferenceVolumeDb (static_cast<float> (referenceSlider.getValue()));
        };
        useVolumeButton.setTooltip ("Make the system volume you have now the volume at which the strips sound as their presets intend");
        useVolumeButton.onClick = [this]
        {
            controller.useCurrentVolumeAsReference();
            refresh();
        };

        // Voice chat (docs/11 E22): the duck's switch and depth, as on the Chat row.
        Style::set (duckToggle, "switch");
        duckToggle.setTitle ("Duck game under voice chat");
        duckToggle.onClick = [this] { controller.setChatDuck (duckToggle.getToggleState(), static_cast<float> (duckDepth.getValue())); };
        duckDepth.setSliderStyle (juce::Slider::LinearHorizontal);
        duckDepth.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
        duckDepth.setRange (EngineController::kMinChatDuckDepthDb, EngineController::kMaxChatDuckDepthDb, 0.5);
        duckDepth.setTextValueSuffix (" dB");
        duckDepth.setTitle ("Chat duck depth");
        duckDepth.onValueChange = [this]
        {
            if (! refreshing)
                controller.setChatDuck (duckToggle.getToggleState(), static_cast<float> (duckDepth.getValue()));
        };
        // The experimental neural voice cleanup on the Chat strip (docs/03 §16).
        Style::set (neuralToggle, "switch");
        neuralToggle.setTitle ("Neural voice cleanup (experimental)");
        neuralToggle.onClick = [this]
        {
            controller.setChatNeuralCleanup (neuralToggle.getToggleState());
            refresh();
        };
        // Its live status, in a fixed-height line of its own: the text changes
        // at 2 Hz while the model runs and must not re-lay out the page.
        neuralStatus.setTitle ("Neural voice cleanup status");
        neuralStatus.setFont (Theme::font (11.5f));
        neuralStatus.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.85f));
        neuralStatus.setJustificationType (juce::Justification::topLeft);
        neuralStatus.setBorderSize ({ 0, 0, 0, 0 });
        neuralStatus.setInterceptsMouseClicks (false, false);

        paletteBox.addItem ("Standard (green / amber / red)", 1);
        paletteBox.addItem ("Colour-blind safe (blue / yellow / vermillion)", 2);
        paletteBox.setSelectedId (palette == MeterPalette::ColourBlindSafe ? 2 : 1, juce::dontSendNotification);
        paletteBox.setTitle ("Meter colours");
        paletteBox.onChange = [this]
        {
            if (onPaletteChanged != nullptr)
                onPaletteChanged (paletteBox.getSelectedId() == 2 ? MeterPalette::ColourBlindSafe : MeterPalette::Standard);
        };

        for (auto* box : { &latencyBox, &protectionBox, &paletteBox })
            addAndMakeVisible (*box);
        addAndMakeVisible (autoReduceToggle);
        addAndMakeVisible (preampToggle);
        addAndMakeVisible (preampHotToggle);
        addAndMakeVisible (smartToggle);
        addAndMakeVisible (restoreButton);
        for (auto* control :
             std::initializer_list<juce::Component*> { &contourToggle, &followToggle, &referenceSlider, &useVolumeButton, &contourView })
            addAndMakeVisible (control);
        addAndMakeVisible (duckToggle);
        addAndMakeVisible (duckDepth);
        addAndMakeVisible (neuralToggle);
        addAndMakeVisible (neuralStatus);

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
        form.section ("Protection");
        form.row ("Protection strength", protectionBox,
                  "The safety governor always scales the Boost and macro amounts back when the limiter works too hard or distortion "
                  "gets audible. Normal also governs the preset's own maximizer drive, saturation drive and bass harmonics; Strict "
                  "lets it go down to 0. Every strip; the Boost panel's governor chip shows what it does.",
                  330);
        form.row ({}, preampToggle, describePreamp(), 520);
        preampRow = form.rows.size() - 1;
        form.row ({}, preampHotToggle,
                  "With the automatic preamp on: while the music's own peaks leave no room under the ceiling (a hot master), "
                  "also takes back the preamp's 1 dB allowance and the maximizer's drive, so the limiter stays idle. Hot "
                  "masters play a little quieter; music with room is unchanged. Saved with the preset; off by default.",
                  520);
        form.row ({}, smartToggle,
                  "Scales what the macros add to the music: less Punch and drive on an already loud, limited master, less bass on "
                  "bass-heavy and less air on bright programme; material with room is unchanged. Per strip; off by default.",
                  520);
        form.section ("Listening level");
        form.row ({}, contourToggle,
                  "Adds the bass and treble the ear misses at low volume (ISO 226 equal loudness), more the further the volume is below "
                  "the reference. Saved with the preset; off by default.",
                  520);
        form.row ({}, followToggle, describeFollow(), 520);
        followRow = form.rows.size() - 1;
        form.row ("Reference volume", referenceSlider, {}, 330);
        form.row ({}, useVolumeButton, describeListeningLevel (controller.getListeningLevel()), 200);
        listeningRow = form.rows.size() - 1;
        // The contour's curve at that level sits under the form (resized).
        displayForm.section ("Voice chat");
        displayForm.row ({}, duckToggle,
                         "While someone talks on the Chat strip, the Game and Music strips dip at 1 - 4 kHz so the voice stays clear; the "
                         "footstep band is kept. Off by default; also on the Chat row of the routing panel.",
                         520);
        displayForm.row ("Duck depth", duckDepth, {}, 330);
        displayForm.row ({}, neuralToggle, describeNeural(), 520);
        displayForm.row ({}, neuralStatus, {}, 2000, kNeuralStatusHeight);
        displayForm.section ("Display");
        displayForm.row ("Meter colours", paletteBox, {}, 330);
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

        protectionBox.setSelectedId (static_cast<int> (controller.getProtectionStrength()) + 1, juce::dontSendNotification);
        duckToggle.setToggleState (controller.getChatDuck(), juce::dontSendNotification);
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            duckDepth.setValue (controller.getChatDuckDepthDb(), juce::dontSendNotification);
        }
        duckDepth.setEnabled (controller.getChatDuck());
        neuralToggle.setToggleState (controller.getChatNeuralCleanup(), juce::dontSendNotification);
        neuralStatus.setText ("Status: " + controller.describeChatNeuralCleanup(), juce::dontSendNotification); // repaints itself only
        preampToggle.setButtonText ("Automatic preamp on the " + controller.getStripName (controller.getSelectedStrip()) + " strip");
        preampHotToggle.setButtonText ("... also on hot programme (" + controller.getStripName (controller.getSelectedStrip()) + " strip)");
        preampHotToggle.setEnabled (controller.getSelectedParams().get (AutoPreampOn) >= 0.5f);
        smartToggle.setButtonText ("Smart macros on the " + controller.getStripName (controller.getSelectedStrip()) + " strip");
        smartToggle.setToggleState (controller.getSmartMacros(), juce::dontSendNotification);
        if (const auto text = describePreamp(); text != form.rows[preampRow].help)
        {
            form.rows[preampRow].help = text;
            resized();
            repaint();
        }

        const auto level = controller.getListeningLevel();
        contourToggle.setButtonText ("Loudness contour on the " + controller.getStripName (controller.getSelectedStrip()) + " strip");
        followToggle.setToggleState (level.following, juce::dontSendNotification);
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            referenceSlider.setValue (level.referenceDb.value_or (0.0f), juce::dontSendNotification);
        }
        referenceSlider.setEnabled (level.following);
        useVolumeButton.setEnabled (level.following);
        for (auto [row, text] : { std::pair { followRow, describeFollow() }, std::pair { listeningRow, describeListeningLevel (level) } })
            if (text != form.rows[row].help)
            {
                form.rows[row].help = text;
                resized();
                repaint();
            }
        {
            // The contour's curve as the selected strip's chain designs it now.
            const auto& chain = controller.getChain (controller.getSelectedStrip());
            contourView.setCurve (contourCurve (chain.effectiveValue (ContourOn) >= 0.5f, chain.effectiveValue (ContourReferencePhon),
                                                chain.effectiveValue (ContourLevelDb) + chain.getListeningLevelDb(),
                                                chain.effectiveValue (ContourMaxLiftDb)));
            if (const auto text = describeContourCurve (contourView.getCurve(), chain.getContourTrimDb()); text != contourText)
            {
                contourText = text;
                repaint (contourTextArea);
            }
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
        t << "  =  " << juce::String (li.totalMs + li.captureBufferMs, 1) << " ms (reported)\n";
        t << describeCpuLine (status, controller.getOverloadState());
        // docs/11 E42d: the latest measurement this session (Settings > Audio).
        const auto& measured = controller.getLatencyMeasurement();
        const auto* report = measured.phase != LatencyMeasurer::Phase::Done ? nullptr
                             : measured.through.has_value()               ? &*measured.through
                             : measured.deviceOnly.has_value()            ? &*measured.deviceOnly
                                                                          : nullptr;
        if (report != nullptr && report->ok)
            t << "\nMeasured (Settings > Audio): round trip " << juce::String (report->roundTripMs, 1) << " ms, playback about "
              << juce::String (report->path == latency::Path::DeviceOnly ? report->withEngineMs : report->playbackMs, 1)
              << " ms with Flubsound, confidence " << latency::confidenceName (report->confidence);

        auto captures = describeCaptureStreams (controller.getCaptureStreams());
        if (captures.isEmpty())
            captures = "No per-app capture streams. Endpoint routing and device inputs do not use one.";
        if (t != latencyText || captures != captureText)
        {
            // The capture block wraps its text; the latency block grows by the measured line.
            const bool relayout = captures != captureText || t.contains ("\nMeasured") != latencyText.contains ("\nMeasured");
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
        g.setColour (Palette::text.withAlpha (0.88f));
        g.setFont (Theme::font (13.0f));
        g.drawText ("Contour now", contourCaption, juce::Justification::centredLeft, true);
        g.setColour (Palette::faint.brighter (0.2f));
        g.setFont (Theme::font (11.5f));
        g.drawFittedText (contourText, contourTextArea, juce::Justification::topLeft, 2, 1.0f);
        displayForm.paint (g);
        drawSectionTitle (g, latencyTitle, "Current latency");
        auto r = latencyArea.toFloat();
        g.setColour (Palette::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (Palette::text.withAlpha (0.85f));
        g.setFont (Theme::font (12.0f));
        g.drawFittedText (latencyText, latencyArea.reduced (12, 8), juce::Justification::topLeft, 5, 1.0f);

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
        const int listeningBottom = form.layout (r);
        // The contour's curve (docs/11 E32) closes the Listening level section.
        contourCaption = { r.getX(), listeningBottom, kCaptionWidth, kRowHeight };
        contourView.setBounds (r.getX() + kCaptionWidth, listeningBottom + 2, juce::jmin (440, r.getWidth() - kCaptionWidth), 124);
        contourTextArea = { contourView.getX(), contourView.getBottom() + 3, r.getWidth() - kCaptionWidth, 2 * kTextLine + 2 };
        const int formBottom = displayForm.layout (r.withTop (contourTextArea.getBottom() + 14));
        latencyTitle = { r.getX(), formBottom + 14, r.getWidth(), 22 };
        latencyArea = { r.getX(), latencyTitle.getBottom() + 6, r.getWidth(), (latencyText.contains ("\nMeasured") ? 5 : 4) * kTextLine + 16 };
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
    static constexpr double kMinReferenceDb = -60.0, kMaxReferenceDb = 12.0;

    juce::String describeFollow() const
    {
        return "Off (default): the contour follows the preset alone. On: Flubsound reads the output device's volume a few times a second; "
               "turning it down below the reference volume adds the contour. Many USB and wireless headsets have a volume dial the "
               "system cannot see.";
    }

    /** The neural voice cleanup row's help: what it is (static; the live
        status is the Status line under it). docs/03 §16. */
    static juce::String describeNeural()
    {
        return "Experimental. A small neural network, trained by Flubsound on synthetic speech, turns down steady noise, hum, "
               "typing and background voices between and under the words of the voices on the Chat strip (per frequency band, "
               "never a hard gate). Adds 20 ms to the Chat strip only, at 48 kHz in Balanced or Quality with buffers of up to "
               "480 samples (longer buffers: 25 ms or more, Quality only). Off by default.";
    }

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
    juce::ComboBox latencyBox, protectionBox, paletteBox;
    juce::ToggleButton autoReduceToggle { "Reduce processing load automatically when the CPU overloads" };
    juce::ToggleButton preampToggle { "Automatic preamp" }, preampHotToggle { "... also on hot programme" };
    juce::ToggleButton smartToggle { "Smart macros" }; // docs/11 E34
    juce::ToggleButton duckToggle { "Duck game under voice chat" }; // docs/11 E22
    juce::Slider duckDepth;
    juce::ToggleButton neuralToggle { "Neural voice cleanup on the Chat strip (experimental)" }; // docs/03 §16
    juce::Label neuralStatus;                    // its live status (EngineController::describeChatNeuralCleanup)
    static constexpr int kNeuralStatusHeight = 36; // room for two lines of 11.5 px text at full width: never re-laid out
    juce::ToggleButton contourToggle { "Loudness contour" }, followToggle { "Follow the system volume" };
    juce::Slider referenceSlider;
    juce::TextButton useVolumeButton { "Use current volume" };
    bool refreshing = false;
    ParameterBinder binder; // after the controls it binds
    size_t preampRow = 0, followRow = 0, listeningRow = 0;
    juce::TextButton restoreButton { "Restore" };
    FormLayout form, displayForm; // Latency .. Listening level; Display (below the contour's curve)
    size_t reductionRow = 0; // the "Automatic change" row: its help is the live description
    ContourCurveView contourView;
    juce::String contourText;
    juce::Rectangle<int> contourCaption, contourTextArea;
    static constexpr int kTextLine = 15; // line pitch of the 12 px text blocks

    juce::String latencyText, captureText;
    juce::TextLayout captureLayout; // one wrapped paragraph per capture stream
    juce::Rectangle<int> latencyArea, latencyTitle, captureArea, captureTitle;
};

// =============================================================================
// Routing page
// =============================================================================
/** Per-app routing (the method; docs/11 E47 / R4.5: "Move the app's own
    sound away automatically" and the silent device) and the device input
    (mode, "Input feeds strip" and the multi-strip input map, R4.6 / docs/11
    E48). The page sets its own height and scrolls in the dialog. */
class SettingsDialog::RoutingPage : public juce::Component
{
public:
    explicit RoutingPage (EngineController& c)
        : controller (c)
    {
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
            refresh();
        };

        // docs/11 E47 (R4.5): the doubling guard's automatic fix.
        Style::set (moveAwayToggle, "switch");
        moveAwayToggle.setTitle ("Move the app's own sound away automatically");
        moveAwayToggle.setTooltip ("Windows: when an app Flubsound captures also plays straight to your headset, move that app's own "
                                   "output to the silent device below, so you hear only Flubsound's processed copy. Its own device "
                                   "is set back when you unassign it, switch this off or quit Flubsound; an app that is playing then "
                                   "may need its playback restarted (reload, or pause and play) to be heard again.");
        moveAwayToggle.onClick = [this]
        {
            controller.getRouting().setMoveOriginalAway (moveAwayToggle.getToggleState());
            refresh();
        };
        silentBox.setTitle ("Silent device");
        silentBox.onChange = [this]
        {
            if (refreshing)
                return;
            const int index = silentBox.getSelectedId() - 2;
            controller.getRouting().setSilentEndpointChoice (index >= 0 && index < static_cast<int> (silentItems.size())
                                                                 ? silentItems[static_cast<size_t> (index)]
                                                                 : AppRouting::OutputEndpoint());
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

        // R4.6 / docs/11 E48: one first-channel choice per strip (the deviceInput.map setting).
        for (int s = 0; s < controller.getNumStrips(); ++s)
        {
            auto box = std::make_unique<juce::ComboBox>();
            box->setTitle ("Input map: " + controller.getStripName (s));
            box->onChange = [this]
            {
                if (! refreshing)
                    applyMap();
            };
            addAndMakeVisible (*box);
            mapBoxes.push_back (std::move (box));
        }
        fillButton.setTooltip ("Each strip from the next free input channel: Game 7.1 from 1, Music from 9, Chat from 11, System from 13 "
                               "(the order of Flubsound's Linux sinks and of its PipeWire device)");
        fillButton.onClick = [this]
        {
            std::vector<int> channels;
            for (int s = 0; s < controller.getNumStrips(); ++s)
                channels.push_back (controller.getStripChannels (s));
            controller.setDeviceInputMapChannels (EngineController::consecutiveInputMap (channels));
            refresh();
        };
        clearButton.setTooltip ("No map: the device input feeds the one strip chosen above");
        clearButton.onClick = [this]
        {
            controller.setDeviceInputMapChannels (std::vector<int> (static_cast<size_t> (controller.getNumStrips()), -1));
            refresh();
        };

        for (auto* control : std::initializer_list<juce::Component*> { &routingBox, &moveAwayToggle, &silentBox, &inputModeBox, &inputStripBox,
                                                                      &fillButton, &clearButton })
            addAndMakeVisible (control);

        form.section ("Per-app routing");
        form.row ("Per-app routing", routingBox,
                  routing.canEnumerateApps() ? juce::String ("Unsupported methods are greyed out.")
                                             : juce::String ("Per-app routing is not available on this system."),
                  220);
        form.row ({}, moveAwayToggle, {}, 520);
        moveAwayRow = form.rows.size() - 1;
        form.row ("Silent device", silentBox,
                  "Where a captured app's own sound goes: a device you do not listen to. Automatic picks an S/PDIF / digital "
                  "output, else an HDMI / DisplayPort output nothing else plays to; never the device Flubsound plays to, the "
                  "system default or a virtual cable that feeds Flubsound.",
                  330);
        form.section ("Device input");
        form.row ("Device input", inputModeBox, "Automatic only processes inputs that look like a virtual cable or loopback device, never a microphone.",
                  330);
        form.row ("Input feeds strip", inputStripBox, "Used while the input map below is empty.", 180);
        mapForm.section ("Input map (several strips from one input)");
        for (size_t s = 0; s < mapBoxes.size(); ++s)
            mapForm.row (controller.getStripName (static_cast<int> (s)), *mapBoxes[s], {}, 220);
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        auto& routing = controller.getRouting();

        using M = AppRouting::Method;
        const auto method = routing.getMethod();
        routingBox.setSelectedId (method == M::EndpointRouting ? 2 : (method == M::ProcessCapture ? 3 : (method == M::Disabled ? 4 : 1)),
                                  juce::dontSendNotification);

        // The option and its silent device. (Not while the device list is
        // open: a rebuild would shift the items under the pointer.)
        const bool offered = routing.canMoveOriginalAway();
        moveAwayToggle.setToggleState (routing.getMoveOriginalAway(), juce::dontSendNotification);
        moveAwayToggle.setEnabled (offered);
        if (! silentBox.isPopupActive())
            rebuildSilentItems();
        silentBox.setEnabled (offered);
        if (const auto text = routing.describeMoveAway(); text != form.rows[moveAwayRow].help)
        {
            form.rows[moveAwayRow].help = text;
            resized();
            repaint();
        }

        using Mode = AppSettings::DeviceInputMode;
        const auto mode = controller.getSettings().getDeviceInputMode();
        inputModeBox.setSelectedId (mode == Mode::On ? 2 : (mode == Mode::Off ? 3 : 1), juce::dontSendNotification);
        const int strip = controller.findStrip (controller.getSettings().getDeviceInputStripName());
        inputStripBox.setSelectedId (strip >= 0 ? strip + 1 : 1, juce::dontSendNotification);

        // Input map: "Not fed", or "Inputs N - M" (a mono strip: "Input N") from every input channel of the open device (16 without one).
        const int inputs = inputChannelCount();
        const auto map = controller.getDeviceInputMapChannels();
        bool anyMapped = false;
        for (size_t s = 0; s < mapBoxes.size(); ++s)
        {
            auto& box = *mapBoxes[s];
            const int first = s < map.size() ? map[s] : -1;
            anyMapped = anyMapped || first >= 0;
            if (box.isPopupActive())
                continue; // the list is open: left as it is until it closes
            if (box.getNumItems() != juce::jmax (inputs, first + 1) + 1)
            {
                box.clear (juce::dontSendNotification);
                box.addItem ("Not fed", 1);
                const int width = controller.getStripChannels (static_cast<int> (s));
                for (int ch = 0; ch < juce::jmax (inputs, first + 1); ++ch)
                    box.addItem (width > 1 ? "Inputs " + juce::String (ch + 1) + " - " + juce::String (ch + width) : "Input " + juce::String (ch + 1),
                                 ch + 2);
            }
            box.setSelectedId (first >= 0 ? first + 2 : 1, juce::dontSendNotification);
        }
        clearButton.setEnabled (anyMapped);
        inputStripBox.setEnabled (! anyMapped);
        const auto text = SettingsDialog::describeInputMap (controller);
        if (text != mapText)
        {
            mapText = text;
            resized();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        form.paint (g);
        mapForm.paint (g);
        g.setColour (Palette::faint.brighter (0.2f));
        g.setFont (Theme::font (11.5f));
        g.drawFittedText (mapText, mapTextArea, juce::Justification::topLeft, 6, 1.0f);
    }

    void resized() override
    {
        const auto r = getLocalBounds();
        const int formBottom = form.layout (r);
        const int mapBottom = mapForm.layout (r.withTop (formBottom));
        auto buttons = juce::Rectangle<int> (r.getX() + kCaptionWidth, mapBottom + 2, r.getWidth() - kCaptionWidth, kRowHeight - 4);
        fillButton.setBounds (buttons.removeFromLeft (200));
        buttons.removeFromLeft (8);
        clearButton.setBounds (buttons.removeFromLeft (110));
        const int lines = juce::jmax (1, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), mapText)
                                                                       / juce::jmax (80.0f, static_cast<float> (r.getWidth() - kCaptionWidth)))));
        mapTextArea = { r.getX() + kCaptionWidth, fillButton.getBottom() + 6, r.getWidth() - kCaptionWidth, (lines + 1) * 15 };
        if (const int h = mapTextArea.getBottom() + 8; h != getHeight())
            setSize (getWidth(), h);
    }

private:
    int inputChannelCount() const { return SettingsDialog::inputMapChannelCount (controller); }

    void rebuildSilentItems()
    {
        auto& routing = controller.getRouting();
        const auto& known = routing.getKnownOutputEndpoints();
        const auto chosen = routing.getSilentEndpointChoice();
        const auto target = routing.getSilentTarget();

        std::vector<AppRouting::OutputEndpoint> items = known;
        bool chosenListed = chosen.id.empty();
        for (const auto& e : known)
            chosenListed = chosenListed || e.id == chosen.id;
        if (! chosenListed)
            items.push_back (chosen);

        // "Automatic (Digital Audio (S/PDIF))": what the automatic choice picks now.
        juce::String automatic ("Automatic");
        if (chosen.id.empty() && ! target.id.empty())
            automatic << " (" << juce::String (target.name) << ")";
        else if (chosen.id.empty() && ! known.empty())
            automatic << " (none found)";
        if (items != silentItems || automatic != silentBox.getItemText (0))
        {
            silentItems = items;
            silentBox.clear (juce::dontSendNotification);
            silentBox.addItem (automatic, 1);
            const auto outputId = routing.getOutputEndpoint().id;
            for (size_t i = 0; i < silentItems.size(); ++i)
            {
                const auto& e = silentItems[i];
                juce::String text (e.name.empty() ? e.id : e.name);
                const bool isOutput = ! outputId.empty() && e.id == outputId;
                const bool isDefault = ! known.empty() && e.id == known.front().id;
                const bool missing = ! chosenListed && e.id == chosen.id;
                if (isOutput)
                    text << " (Flubsound's output)";
                else if (isDefault)
                    text << " (system default)";
                else if (missing)
                    text << " (not connected)";
                silentBox.addItem (text, static_cast<int> (i) + 2);
                silentBox.setItemEnabled (static_cast<int> (i) + 2, ! isOutput && ! isDefault);
            }
        }
        int selected = 1;
        for (size_t i = 0; i < silentItems.size() && ! chosen.id.empty(); ++i)
            if (silentItems[i].id == chosen.id)
                selected = static_cast<int> (i) + 2;
        silentBox.setSelectedId (selected, juce::dontSendNotification);
    }

    void applyMap()
    {
        std::vector<int> channels;
        for (const auto& box : mapBoxes)
            channels.push_back (box->getSelectedId() >= 2 ? box->getSelectedId() - 2 : -1);
        controller.setDeviceInputMapChannels (channels);
        refresh();
    }

    EngineController& controller;
    juce::ComboBox routingBox, silentBox, inputModeBox, inputStripBox;
    juce::ToggleButton moveAwayToggle { "Move the app's own sound away automatically" };
    std::vector<std::unique_ptr<juce::ComboBox>> mapBoxes; // one per strip
    juce::TextButton fillButton { "Fill in one after another" }, clearButton { "Clear map" };
    std::vector<AppRouting::OutputEndpoint> silentItems; // item id - 2
    FormLayout form, mapForm;
    size_t moveAwayRow = 0;
    juce::String mapText;
    juce::Rectangle<int> mapTextArea;
    bool refreshing = false;
};

// =============================================================================
// Hotkeys page
// =============================================================================
/** One row per action: name, the chord (recorded by pressing it,
    HotkeyCapture.h), reset to default, the action's registration status and,
    while it is not active, "Pick a free combination" (R4.4). The summary
    under the rows names every problem with its reason, the recorder's
    prompt and refusals, and what a pick did. Results that arrive later (the
    Wayland portal asks the desktop, which may ask the user) are picked up by
    a 4 Hz poll while the page is visible. */
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
            stopRecording();
            controller.getSettings().setHotkeysEnabled (enabledToggle.getToggleState());
            note = {};
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

            row.field = std::make_unique<HotkeyCaptureField> (row.name);
            auto* field = row.field.get();
            field->onCaptureStarted = [this, action]
            {
                recording = action;
                recordedChange = false;
                note = "Press the new shortcut for " + AppSettings::getHotkeyActionName (action)
                       + " (Ctrl, Alt and Shift with a letter, a digit, F1-F24 or a navigation key). Esc cancels, Backspace clears it. "
                         "A combination another application holds never arrives here.";
                noteIsError = false;
                setSuspended (true);
                updateStatus();
            };
            field->onChordPressed = [this, action] (const flub::platform::KeyChord& chord) { return acceptRecorded (action, chord); };
            field->onCleared = [this, action]
            {
                controller.getSettings().setHotkey (action, {});
                note = AppSettings::getHotkeyActionName (action) + " has no shortcut now.";
                noteIsError = false;
                recordedChange = true;
            };
            field->onUnsupportedKey = [this] (const juce::String& key)
            {
                note = "\"" + key + "\" cannot be part of a hotkey: use a letter, a digit, F1-F24, Space or a navigation key, with Ctrl or Alt. "
                       "Esc cancels; right-click or Shift+F10 types a chord instead.";
                noteIsError = true;
                updateStatus();
            };
            field->onCaptureEnded = [this]
            {
                recording.reset();
                if (! recordedChange)
                {
                    note = {}; // cancelled: nothing changed
                    noteIsError = false;
                }
                setSuspended (false); // registers everything again, with the new chord
                refreshFields();
                updateStatus();
            };
            addAndMakeVisible (*row.field);

            // A right click on the field types the chord instead (Win / Super
            // key chords, which JUCE cannot record on Windows and Linux).
            row.editor = std::make_unique<juce::TextEditor>();
            row.editor->setTitle (row.name + " shortcut as text");
            row.editor->setFont (Theme::font (13.0f));
            row.editor->setJustification (juce::Justification::centredLeft);
            row.editor->setIndents (8, 0);
            row.editor->setTooltip ("Type a chord such as Ctrl+Alt+F, Super+F5 or None, then press Return. Esc cancels.");
            row.editor->onReturnKey = [this, action] { commitTyped (action); };
            row.editor->onFocusLost = [this, action] { commitTyped (action); };
            row.editor->onEscapeKey = [this, action] { closeTyped (action); };
            addChildComponent (*row.editor);
            field->onTypeRequested = [this, action] { openTyped (action); };

            row.reset = std::make_unique<IconButton> ("Reset " + row.name + " to default", Icons::reset(), IconButton::Style::Framed);
            row.reset->setTooltip ("Default: " + AppSettings::chordToString (AppSettings::getDefaultHotkey (action)));
            row.reset->onClick = [this, action]
            {
                stopRecording(); // the button takes no focus, so the field would go on recording
                const auto chord = AppSettings::getDefaultHotkey (action);
                controller.getSettings().setHotkey (action, chord);
                note = AppSettings::getHotkeyActionName (action) + " is back to its default, " + AppSettings::chordToString (chord) + ".";
                noteIsError = false;
                reRegister();
            };
            addAndMakeVisible (*row.reset);

            row.pick = std::make_unique<juce::TextButton> ("Pick a free one");
            row.pick->setTitle ("Pick a free combination for " + row.name);
            row.pick->onClick = [this, action] { pickFree (action); };
            addChildComponent (*row.pick);
            rows.push_back (std::move (row));
        }
        refresh();
    }

    ~HotkeysPage() override
    {
        // Closed while recording (the window's close button): the hotkeys
        // must not stay suspended.
        for (auto& row : rows)
        {
            row.field->onCaptureEnded = nullptr;
            row.editor->onFocusLost = nullptr;
        }
        if (recording.has_value())
            setSuspended (false);
    }

    void refresh()
    {
        if (! recording.has_value())
        {
            note = {}; // the page shown again: the state, not an old outcome
            noteIsError = false;
        }
        refreshFields();
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

    /** The text under the rows (tests). */
    const juce::String& getSummary() const noexcept { return status; }

    void visibilityChanged() override
    {
        if (isVisible())
            startTimerHz (4);
        else
        {
            stopTimer();
            for (auto& row : rows)
            {
                row.field->cancelCapture();
                closeTyped (row.action);
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "System-wide shortcuts");
        g.setColour (Palette::text.withAlpha (0.88f));
        g.setFont (Theme::font (13.0f));
        g.drawText ("Hotkeys act on", stripCaptionArea, juce::Justification::centredLeft, true);
        const auto colours = Theme::statusColours (*this);
        for (const auto& row : rows)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.setFont (Theme::font (13.0f));
            g.drawText (row.name, row.captionArea, juce::Justification::centredLeft, true);

            using S = HotkeyManager::Status;
            const auto state = row.state.status;
            g.setColour (HotkeyManager::isProblem (state) ? colours.hot : state == S::Registered ? Palette::green : Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (row.stateText, row.stateArea, juce::Justification::centredLeft, 2, 0.8f);
        }
        if (status.isNotEmpty())
        {
            g.setColour (statusIsError ? colours.hot : Palette::faint.brighter (0.2f));
            g.setFont (Theme::font (12.0f));
            g.drawFittedText (status, statusArea, juce::Justification::topLeft, 9, 1.0f);
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
            stripCaptionArea = line.removeFromLeft (140);
            stripBox.setBounds (line.removeFromLeft (140).reduced (0, 2));
        }
        r.removeFromTop (6);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (kRowHeight);
            row.captionArea = line.removeFromLeft (140);
            row.field->setBounds (line.removeFromLeft (140).reduced (0, 2));
            row.editor->setBounds (row.field->getBounds());
            line.removeFromLeft (6);
            row.reset->setBounds (line.removeFromLeft (26).reduced (0, 2));
            line.removeFromLeft (8);
            // "Pick a free one" at the right of a row that is not active; the
            // status text keeps the rest (two lines).
            const int pickWidth = juce::jlimit (84, 132, line.getWidth() / 2);
            row.pick->setButtonText (pickWidth >= 116 ? "Pick a free one" : "Pick free");
            row.pick->setBounds (line.removeFromRight (pickWidth).reduced (0, 3));
            line.removeFromRight (6);
            row.stateArea = row.pick->isVisible() ? line : line.withRight (row.pick->getRight());
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
        std::unique_ptr<HotkeyCaptureField> field;
        std::unique_ptr<juce::TextEditor> editor; // the typed alternative, in the field's place while open
        std::unique_ptr<IconButton> reset;
        std::unique_ptr<juce::TextButton> pick; // shown while the action's hotkey is not active
        juce::Rectangle<int> captionArea, stateArea;
        HotkeyManager::ActionStatus state;
        juce::String stateText; // HotkeyManager::describe, "" without the hook
    };

    void timerCallback() override
    {
        // Only a change rewrites the summary, so a refusal stays until the
        // next change.
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

    void refreshFields()
    {
        for (auto& row : rows)
            if (! row.field->isCapturing())
                row.field->setChordText (AppSettings::chordToString (controller.getSettings().getHotkey (row.action)));
    }

    /** A recorded chord: refused (recording goes on) when it is not a valid
        hotkey, would take a key other applications use
        (HotkeyManager::commonShortcutProblem) or another action has it;
        otherwise saved. */
    bool acceptRecorded (HotkeyAction action, const flub::platform::KeyChord& chord)
    {
        auto& settings = controller.getSettings();
        const auto text = AppSettings::chordToString (chord);
        if (const auto why = HotkeyManager::validateChord (chord); why.isNotEmpty())
        {
            note = text + " cannot be a hotkey: " + why + " Press another combination, or Esc.";
            noteIsError = true;
            updateStatus();
            return false;
        }
        if (const auto why = HotkeyManager::commonShortcutProblem (chord, true); why.isNotEmpty())
        {
            note = "Not taken: " + why + " Press another combination, or Esc.";
            noteIsError = true;
            updateStatus();
            return false;
        }
        if (const auto other = HotkeyManager::findConflict (settings, action, chord))
        {
            note = text + " is already " + AppSettings::getHotkeyActionName (*other) + "'s shortcut. Press another combination, or Esc to keep "
                   + AppSettings::chordToString (settings.getHotkey (action)) + ".";
            noteIsError = true;
            updateStatus();
            return false;
        }
        settings.setHotkey (action, chord);
        note = AppSettings::getHotkeyActionName (action) + " is now " + text + ".";
        noteIsError = false;
        recordedChange = true;
        return true;
    }

    Row* rowOf (HotkeyAction action)
    {
        for (auto& row : rows)
            if (row.action == action)
                return &row;
        return nullptr;
    }

    void openTyped (HotkeyAction action)
    {
        auto* row = rowOf (action);
        if (row == nullptr)
            return;
        stopRecording();
        typing = action;
        row->editor->setText (AppSettings::chordToString (controller.getSettings().getHotkey (action)), false);
        row->field->setVisible (false);
        row->editor->setVisible (true);
        if (row->editor->isShowing())
            row->editor->grabKeyboardFocus();
        row->editor->selectAll();
    }

    void closeTyped (HotkeyAction action)
    {
        auto* row = rowOf (action);
        if (row == nullptr || typing != action)
            return;
        typing.reset(); // first: hiding the editor takes its focus (onFocusLost)
        row->editor->setVisible (false);
        row->field->setVisible (true);
    }

    /** The typed chord: "None" or empty clears it; otherwise saved when it is
        a valid hotkey no other action has. */
    void commitTyped (HotkeyAction action)
    {
        auto* row = rowOf (action);
        if (row == nullptr || typing != action)
            return;
        auto& settings = controller.getSettings();
        const auto name = AppSettings::getHotkeyActionName (action);
        const auto text = row->editor->getText().trim();
        flub::platform::KeyChord chord;
        noteIsError = true;
        if (text.isEmpty() || text.equalsIgnoreCase ("None"))
        {
            settings.setHotkey (action, {});
            note = name + " has no shortcut now.";
            noteIsError = false;
        }
        else if (! AppSettings::chordFromString (text, chord))
            note = "\"" + text + "\" is not a shortcut. Use modifiers + one key, e.g. Ctrl+Alt+F or Super+F5.";
        else if (const auto why = HotkeyManager::validateChord (chord); why.isNotEmpty())
            note = AppSettings::chordToString (chord) + " cannot be a hotkey: " + why;
        else if (const auto taken = HotkeyManager::commonShortcutProblem (chord, false); taken.isNotEmpty())
            note = "Not taken: " + taken;
        else if (const auto other = HotkeyManager::findConflict (settings, action, chord))
            note = AppSettings::chordToString (chord) + " is already " + AppSettings::getHotkeyActionName (*other) + "'s shortcut.";
        else
        {
            settings.setHotkey (action, chord);
            note = name + " is now " + AppSettings::chordToString (chord) + ".";
            noteIsError = false;
        }
        closeTyped (action);
        reRegister();
    }

    void pickFree (HotkeyAction action)
    {
        if (hooks.pickFreeChord == nullptr)
            return;
        stopRecording();
        const auto result = hooks.pickFreeChord (action);
        note = result.message;
        noteIsError = ! result.found;
        refreshFields();
        updateStatus();
    }

    /** Ends a recording before another control changes or registers the
        hotkeys: the reset button and the switch take no keyboard focus, so
        the field would go on recording while its chords are registered
        again, and a chord recorded then would be saved but not registered.
        Cancelling resumes the hotkeys (onCaptureEnded); the caller's own
        registration follows in the same message, which the Wayland portal
        coalesces into one binding. */
    void stopRecording()
    {
        for (auto& row : rows)
            row.field->cancelCapture();
    }

    void setSuspended (bool suspended)
    {
        if (hooks.setSuspended != nullptr)
            hooks.setSuspended (suspended);
        else if (! suspended && hooks.reRegister != nullptr)
            hooks.reRegister();
    }

    void reRegister()
    {
        stopRecording();
        if (hooks.reRegister != nullptr)
            hooks.reRegister();
        else
            setSuspended (false);
        refreshFields();
        updateStatus();
    }

    void updateStatus()
    {
        using S = HotkeyManager::Status;
        int pending = 0, reassigned = 0;
        juce::StringArray problems;
        for (auto& row : rows)
        {
            const auto latest = readState (row.action);
            row.state = latest.state;
            row.stateText = latest.text;
            pending += row.state.status == S::Pending ? 1 : 0;
            reassigned += row.state.status == S::Reassigned ? 1 : 0;
            const bool problem = HotkeyManager::isProblem (row.state.status);
            if (problem)
                problems.add (HotkeyManager::describeFailure ({ row.action, row.state }) + ".");
            row.field->setProblem (problem);
            row.pick->setVisible (problem && hooks.pickFreeChord != nullptr);
            if (problem)
                row.pick->setTooltip ("Tries " + describeCandidates (row.action)
                                      + " in turn and keeps the first one no other application holds.");
        }
        resized(); // the status text takes the room of a hidden pick button

        const bool supported = hooks.isSupported != nullptr && hooks.isSupported();
        juce::String state;
        bool stateIsError = false;
        if (recording.has_value())
        {
            state = {};
        }
        else if (! supported)
        {
            state = "System-wide hotkeys are not available here (no platform support, or a Wayland session without the "
                    "GlobalShortcuts portal). The shortcuts are saved and apply where supported.";
        }
        else if (! controller.getSettings().getHotkeysEnabled())
        {
            state = "Shortcuts are switched off.";
        }
        else if (! problems.isEmpty())
        {
            state = "Not active: " + problems.joinIntoString (" ")
                    + " Pick a free combination, or click the shortcut and press a new one.";
            stateIsError = true;
        }
        else if (const auto failures = hooks.getStatus == nullptr && hooks.getFailures != nullptr ? hooks.getFailures() : juce::StringArray();
                 ! failures.isEmpty())
        {
            state = "Could not register: " + failures.joinIntoString ("; ") + ".";
            stateIsError = true;
        }
        else if (pending > 0)
            state = "Waiting for the desktop to confirm the shortcuts; it may ask you in a dialog.";
        else if (reassigned > 0)
            state = "The desktop bound some shortcuts to other keys (shown next to each); change them in its keyboard settings.";
        else
            state = "All shortcuts are registered.";

        status = note.isNotEmpty() && state.isNotEmpty() ? note + "\n" + state : note + state;
        statusIsError = noteIsError || (note.isEmpty() && stateIsError);
        repaint();
    }

    juce::String describeCandidates (HotkeyAction action) const
    {
        juce::StringArray names;
        for (const auto& chord : HotkeyManager::freeChordCandidates (controller.getSettings(), action))
            names.add (AppSettings::chordToString (chord));
        return names.isEmpty() ? juce::String ("the alternatives") : names.joinIntoString (", ");
    }

    EngineController& controller;
    HotkeyHooks hooks;
    juce::ToggleButton enabledToggle { "Enable system-wide hotkeys" };
    juce::ComboBox stripBox;
    juce::Rectangle<int> stripCaptionArea;
    std::vector<Row> rows;
    std::optional<HotkeyAction> recording; // the row whose chord is being recorded
    std::optional<HotkeyAction> typing;    // the row whose chord is being typed
    bool recordedChange = false;           // this recording saved or cleared a chord
    juce::String note;                     // the last edit's outcome, a refusal or the recorder's prompt
    bool noteIsError = false;
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
// Diagnostics page (docs/11 E54)
// =============================================================================
class SettingsDialog::DiagnosticsPage : public juce::Component
{
public:
    explicit DiagnosticsPage (EngineController& c)
        : controller (c)
    {
        revealLogs.setText ("Show");
        revealLogs.onClick = []
        {
            const auto folder = diagnostics::logFolder();
            folder.createDirectory();
            folder.revealToUser();
        };
        addAndMakeVisible (revealLogs);

        exportButton.setButtonText ("Export diagnostics...");
        exportButton.setTooltip ("Save one zip to attach to a report: versions, the audio device and its state, settings, logs and crash reports");
        exportButton.onClick = [this] { chooseAndExport(); };
        addAndMakeVisible (exportButton);

        // docs/11 E54: the notify-only update check, off by default.
        Style::set (updateToggle, "switch");
        updateToggle.setTooltip ("Ask GitHub at start (at most once a day) whether a newer Flubsound release exists, and say so with a "
                                 "link to its page. Nothing is downloaded or installed.");
        updateToggle.onClick = [this]
        {
            auto& file = controller.getSettings().getPropertiesFile();
            diagnostics::update::setEnabled (file, updateToggle.getToggleState());
            if (updateToggle.getToggleState())
                checkNow(); // switching it on is the moment to look
            refresh();
        };
        addAndMakeVisible (updateToggle);
        channelBox.addItem ("Stable releases", 1);
        channelBox.addItem ("Beta: pre-releases too", 2);
        channelBox.setTitle ("Update channel");
        channelBox.onChange = [this]
        {
            diagnostics::update::setChannel (controller.getSettings().getPropertiesFile(), channelBox.getSelectedId() == 2
                                                                                                ? diagnostics::update::Channel::Beta
                                                                                                : diagnostics::update::Channel::Stable);
        };
        addAndMakeVisible (channelBox);
        checkButton.setButtonText ("Check now");
        checkButton.onClick = [this] { checkNow(); };
        addAndMakeVisible (checkButton);
        downloadLink.setButtonText ("Open the download page");
        downloadLink.setFont (Theme::font (12.5f), false, juce::Justification::centredLeft);
        addChildComponent (downloadLink);
        refresh();
    }

    void refresh()
    {
        crashText = describeCrashReports (diagnostics::logFolder());
        const auto& file = controller.getSettings().getPropertiesFile();
        const bool on = diagnostics::update::isEnabled (file);
        updateToggle.setToggleState (on, juce::dontSendNotification);
        channelBox.setSelectedId (diagnostics::update::getChannel (file) == diagnostics::update::Channel::Beta ? 2 : 1,
                                  juce::dontSendNotification);
        const bool checking = checker != nullptr && checker->isChecking();
        checkButton.setEnabled (on && ! checking);
        checkButton.setButtonText (checking ? "Checking..." : "Check now");
        updateText = describeUpdateCheck (file);
        const auto url = on ? diagnostics::update::getLastResultUrl (file) : juce::String();
        downloadLink.setURL (juce::URL (url));
        downloadLink.setVisible (url.isNotEmpty());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        drawSectionTitle (g, titleArea, "Diagnostics");
        g.setColour (Palette::faint.brighter (0.2f));
        g.setFont (Theme::font (12.0f));
        g.drawFittedText ("Flubsound keeps a log of device changes, errors, glitches and overloads, and writes a crash report if it "
                          "crashes. They stay on this computer and contain no audio. To report a problem, export them and attach the zip.",
                          introArea, juce::Justification::topLeft, 3, 1.0f);
        g.setFont (Theme::font (12.5f));
        auto line = [&g] (juce::Rectangle<int> area, const juce::String& caption, const juce::String& value)
        {
            g.setColour (Palette::text.withAlpha (0.88f));
            g.drawText (caption, area.removeFromLeft (kCaptionWidth), juce::Justification::centredLeft, true);
            g.setColour (Palette::muted);
            g.drawFittedText (value, area, juce::Justification::centredLeft, 1, 0.7f);
        };
        line (folderLine, "Log folder", diagnostics::logFolder().getFullPathName());
        line (crashLine, "Crash reports", crashText);
        if (status.isNotEmpty())
        {
            g.setColour (statusIsError ? Palette::amber : Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawFittedText (status, statusArea, juce::Justification::topLeft, 3, 1.0f);
        }

        drawSectionTitle (g, updatesTitle, "Updates");
        g.setColour (Palette::text.withAlpha (0.88f));
        g.setFont (Theme::font (12.5f));
        g.drawText ("Channel", channelLine, juce::Justification::centredLeft, true);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (12.0f));
        g.drawFittedText (updateText, updateTextArea, juce::Justification::topLeft, 2, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleArea = r.removeFromTop (22);
        r.removeFromTop (10);
        introArea = r.removeFromTop (50);
        r.removeFromTop (6);
        folderLine = r.removeFromTop (kRowHeight);
        revealLogs.setBounds (folderLine.removeFromRight (70).reduced (0, 2));
        folderLine.removeFromRight (8);
        r.removeFromTop (4);
        crashLine = r.removeFromTop (kRowHeight);
        r.removeFromTop (14);
        exportButton.setBounds (r.removeFromTop (kRowHeight).withWidth (220).reduced (0, 2));
        r.removeFromTop (6);
        statusArea = r.removeFromTop (48);

        r.removeFromTop (8);
        updatesTitle = r.removeFromTop (22);
        r.removeFromTop (8);
        updateToggle.setBounds (r.removeFromTop (26).withWidth (juce::jmin (r.getWidth(), 420)));
        r.removeFromTop (4);
        channelLine = r.removeFromTop (kRowHeight);
        channelBox.setBounds (channelLine.withTrimmedLeft (kCaptionWidth).withWidth (220).reduced (0, 3));
        checkButton.setBounds (channelBox.getBounds().translated (232, 0).withWidth (120));
        r.removeFromTop (6);
        updateTextArea = r.removeFromTop (34);
        downloadLink.setBounds (r.removeFromTop (24).withWidth (220));
    }

private:
    void checkNow()
    {
        if (checker == nullptr)
        {
            checker = std::make_unique<diagnostics::update::UpdateChecker> (controller.getSettings().getPropertiesFile(),
                                                                             diagnostics::update::UpdateChecker::Options());
            checker->onResult = [this] (const diagnostics::update::Result&) { refresh(); };
        }
        checker->checkNow();
        refresh();
    }

    void chooseAndExport()
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
        chooser = std::make_unique<juce::FileChooser> ("Save diagnostics", folder.getChildFile (diagnostics::defaultBundleName()), "*.zip");
        juce::Component::SafePointer<DiagnosticsPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  auto target = fc.getResult();
                                  if (! target.hasFileExtension ("zip"))
                                      target = target.withFileExtension ("zip");
                                  const auto error = exportDiagnostics (safe->controller, diagnostics::logFolder(), target);
                                  safe->statusIsError = error.isNotEmpty();
                                  safe->status = error.isNotEmpty() ? error : "Saved " + target.getFullPathName();
                                  if (error.isEmpty())
                                      target.revealToUser();
                                  safe->refresh();
                              });
    }

    EngineController& controller;
    IconButton revealLogs { "Show the log folder", Icons::external(), IconButton::Style::Framed };
    juce::TextButton exportButton;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String crashText, status;
    bool statusIsError = false;
    juce::Rectangle<int> titleArea, introArea, folderLine, crashLine, statusArea;
    // Updates (docs/11 E54)
    juce::ToggleButton updateToggle { "Check for updates (notify only)" };
    juce::ComboBox channelBox;
    juce::TextButton checkButton;
    juce::HyperlinkButton downloadLink;
    juce::String updateText;
    juce::Rectangle<int> updatesTitle, channelLine, updateTextArea;
    std::unique_ptr<diagnostics::update::UpdateChecker> checker; // "Check now"; destroyed (and cancelled) with the page
};

// =============================================================================
// SettingsDialog
// =============================================================================
SettingsDialog::SettingsDialog (EngineController& c, HotkeyHooks hooks, std::function<void (MeterPalette)> onPalette, MeterPalette palette)
    : controller (c)
{
    setTitle ("Flubsound settings");

    static const char* names[] = { "Audio", "Correction", "Processing", "Routing", "Hearing", "Hotkeys", "General", "Diagnostics" };
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
    routingPage = std::make_unique<RoutingPage> (controller); // docs/11 E47 (R4.5), E48 (R4.6)
    hearingPage = std::make_unique<HearingPage> (controller); // docs/11 E32 (c), E33
    hotkeysPage = std::make_unique<HotkeysPage> (controller, std::move (hooks));
    generalPage = std::make_unique<GeneralPage> (controller);
    diagnosticsPage = std::make_unique<DiagnosticsPage> (controller);
    audioView.setViewedComponent (audioPage.get(), false);
    processingView.setViewedComponent (processingPage.get(), false);
    routingView.setViewedComponent (routingPage.get(), false);
    hearingView.setViewedComponent (hearingPage.get(), false);
    for (auto* view : { &audioView, &processingView, &routingView, &hearingView })
    {
        view->setScrollBarsShown (true, false);
        view->setScrollBarThickness (8);
        addChildComponent (*view);
    }
    addChildComponent (*correctionPage);
    addChildComponent (*hotkeysPage);
    addChildComponent (*generalPage);
    addChildComponent (*diagnosticsPage);

    showPage (Page::Audio);
    setSize (780, 600);
    startTimerHz (2);
}

SettingsDialog::~SettingsDialog()
{
    stopTimer();
    audioView.setViewedComponent (nullptr, false);
    processingView.setViewedComponent (nullptr, false);
    routingView.setViewedComponent (nullptr, false);
    hearingView.setViewedComponent (nullptr, false);
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

AppSettings::LoopbackPair SettingsDialog::loopbackPairToAllow (EngineController& controller)
{
    const auto state = controller.getDeviceSafetyState();
    if (state.kind == DeviceSafetyState::Kind::LoopbackPair)
        return { state.inputDeviceName, state.outputDeviceName };
    const auto setup = controller.getDeviceManager().getAudioDeviceSetup();
    if (setup.inputDeviceName.isEmpty() || setup.outputDeviceName.isEmpty() || ! AudioEngineHost::isLoopbackPair (setup.inputDeviceName, setup.outputDeviceName))
        return {};
    for (const auto& p : controller.getAllowedLoopbackPairs())
        if (p.input.equalsIgnoreCase (setup.inputDeviceName) && p.output.equalsIgnoreCase (setup.outputDeviceName))
            return {};
    return { setup.inputDeviceName, setup.outputDeviceName };
}

juce::String SettingsDialog::describeDeviceTypeNote (const juce::String& deviceTypeName)
{
    // docs/08 D10: an ASIO driver usually takes one client, and the output
    // device is then Flubsound's alone.
    if (deviceTypeName == "ASIO")
        return "ASIO drivers usually serve one application at a time: while Flubsound plays through this ASIO device, other apps "
               "cannot use it directly. Send them to Flubsound's virtual devices so Flubsound plays everything. The driver's own "
               "control panel may also fix the sample rate and buffer size.";
    // docs/08 D2: the same for WASAPI exclusive mode.
    if (deviceTypeName == "Windows Audio (Exclusive Mode)")
        return "Exclusive mode gives this output to Flubsound alone: other apps cannot play to it while Flubsound runs. Send them to "
               "Flubsound's virtual devices, or choose \"Windows Audio\" or \"Windows Audio (Low Latency Mode)\" to share the output.";
    // docs/11 E48: the native node; its single device is not a sound card.
    if (deviceTypeName == "PipeWire")
        return "Flubsound's own PipeWire node: it creates the Game, Music, Chat and System sinks, reads them and plays to the default "
               "output, linking everything itself (no setup script or manual links). Send apps to those sinks in the routing panel "
               "or your desktop's sound settings.";
    return {};
}

juce::String SettingsDialog::getAudioDeviceTypeNote()
{
    if (audioPage == nullptr)
        return {};
    audioPage->refresh();
    return audioPage->getTypeNote();
}

juce::String SettingsDialog::describeLoopbackGuard (EngineController& controller)
{
    const auto state = controller.getDeviceSafetyState();
    if (state.kind == DeviceSafetyState::Kind::LoopbackPair)
        return "Output muted: \"" + state.outputDeviceName + "\" plays back into the input \"" + state.inputDeviceName
               + "\" (a feedback loop). Choose another output below, or allow the pair if you route it on purpose.";
    const auto setup = controller.getDeviceManager().getAudioDeviceSetup();
    if (setup.inputDeviceName.isNotEmpty() && setup.outputDeviceName.isNotEmpty()
        && AudioEngineHost::isLoopbackPair (setup.inputDeviceName, setup.outputDeviceName))
    {
        for (const auto& p : controller.getAllowedLoopbackPairs())
            if (p.input.equalsIgnoreCase (setup.inputDeviceName) && p.output.equalsIgnoreCase (setup.outputDeviceName))
                return "The current pair looks like a feedback loop and is allowed: Flubsound plays it.";
        return "The current input and output look like the two ends of one cable. The output is muted if the input feeds a strip.";
    }
    return "Flubsound mutes the output when it would play back into the input it processes (the two ends of a virtual cable, "
           "a sink and its monitor), e.g. after a wireless headset disconnects. Pairs you allow play anyway.";
}

int SettingsDialog::inputMapChannelCount (EngineController& controller)
{
    if (auto* device = controller.getDeviceManager().getCurrentAudioDevice())
        if (const int active = device->getActiveInputChannels().countNumberOfSetBits(); active > 0)
            return active;
    return 16;
}

juce::String SettingsDialog::describeInputMap (EngineController& controller)
{
    bool anyMapped = false;
    for (const int first : controller.getDeviceInputMapChannels())
        anyMapped = anyMapped || first >= 0;
    // Whether the device input feeds any strip now (EngineController's
    // policy: Off, or Automatic with an input that is no virtual cable /
    // loopback device, processes none of it, and then no map applies).
    bool processed = false;
    for (const int first : controller.getHost().getDeviceInputMap())
        processed = processed || first >= 0;

    using Mode = AppSettings::DeviceInputMode;
    juce::String t;
    if (! processed)
        t << (controller.getSettings().getDeviceInputMode() == Mode::Off
                  ? juce::String ("Device input is off")
                  : juce::String ("Device input is not processed now (Automatic takes only an input that looks like a virtual cable or "
                                  "loopback device; choose Always to process this one)"))
          << (anyMapped ? ": the map is not used. " : ". ");
    else if (anyMapped)
        t << "The map is in use: each strip reads its channels from the one chosen, and \"Input feeds strip\" is not used. ";
    else
        t << "Empty: the device input feeds the one strip chosen above. ";
    t << "Use it when one input carries several strips (e.g. a 14-channel JACK / PipeWire input, or a virtual mixer's outputs). ";
   #if JUCE_LINUX
    t << "On Linux Flubsound links each flubsound_<strip> sink's monitor to these inputs itself (pw-link), so no qpwgraph step "
         "is needed; Flubsound's own PipeWire device needs no map. ";
   #endif
    const int inputs = inputMapChannelCount (controller);
    if (auto* device = controller.getDeviceManager().getCurrentAudioDevice())
        t << device->getName() << ": " << inputs << " input channel" << (inputs == 1 ? "" : "s") << " active.";
    else
        t << "No device is open: up to 16 inputs are offered.";
    return t;
}

juce::String SettingsDialog::describeListeningLevel (const EngineController::ListeningLevel& level)
{
    if (! level.following)
        return "Off: the contour follows the preset's Listening Level alone.";
    juce::String t;
    if (level.known)
        t << "System volume now " << Theme::formatSignedDb (level.volumeDb) << " dB" << (level.muted ? " (muted)" : "");
    else
        t << "The system volume cannot be read" << (level.error.isNotEmpty() ? " (" + level.error + ")" : juce::String());
    if (level.referenceDb.has_value())
        t << ", reference " << Theme::formatSignedDb (*level.referenceDb) << " dB";
    return t << ": the contour plays " << Theme::formatSignedDb (level.levelDb) << " dB re the reference.";
}

SettingsDialog::ContourCurve SettingsDialog::contourCurve (bool on, float referencePhon, float levelDb, float maxLiftDb)
{
    // As LoudnessContour::design() targets it: the level clamped the same
    // way, the lift capped at contour.maxLift.
    ContourCurve curve;
    curve.on = on;
    if (! on)
        return curve;
    flub::LoudnessContourParams p;
    p.enabled = true;
    p.referencePhon = referencePhon;
    p.levelDb = levelDb;
    p.maxLiftDb = maxLiftDb;
    curve.levelDb = flub::LoudnessContour::effectiveLevelDb (p);
    const double reference = std::isfinite (referencePhon) ? std::clamp (static_cast<double> (referencePhon), 20.0, 100.0) : 80.0;
    const double cap = std::isfinite (maxLiftDb) ? std::max (0.0, static_cast<double> (maxLiftDb)) : 0.0;
    for (int i = 0; i < flub::iso226::kNumFrequencies; ++i)
        curve.liftDb[static_cast<size_t> (i)] =
            static_cast<float> (std::min (flub::iso226::relativeGainDb (i, reference, static_cast<double> (curve.levelDb)), cap));
    return curve;
}

juce::String SettingsDialog::describeContourCurve (const ContourCurve& curve, float trimDb)
{
    if (! curve.on)
        return "Off: switch the loudness contour on above to see what it adds at your listening level.";
    // The lift at 50 Hz and the largest one above 1 kHz, the two ends a listener hears.
    const auto& f = flub::iso226::kFrequencies;
    size_t at50 = 0, treble = 0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        if (std::abs (f[i] - 50.0) < 1.0)
            at50 = i;
        if (f[i] > 1000.0 && (treble == 0 || curve.liftDb[i] > curve.liftDb[treble]))
            treble = i;
    }
    const auto hz = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz"; };
    juce::String t;
    t << "At " << Theme::formatSignedDb (curve.levelDb) << " dB re the reference: " << Theme::formatSignedDb (curve.liftDb[at50]) << " dB at 50 Hz, "
      << Theme::formatSignedDb (curve.liftDb[treble]) << " dB at " << hz (f[treble]);
    if (trimDb < -0.05f)
        t << "; level trim " << Theme::formatSignedDb (trimDb) << " dB (so the lift does not drive the limiter)";
    return t << ".";
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

juce::String SettingsDialog::describeCrashReports (const juce::File& logFolder)
{
    const auto reports = diagnostics::crash::findReports (logFolder);
    if (reports.isEmpty())
        return "None";
    return juce::String (reports.size()) + "  -  the latest on " + reports.getFirst().getLastModificationTime().formatted ("%Y-%m-%d %H:%M");
}

juce::String SettingsDialog::describeUpdateCheck (const juce::PropertiesFile& settings)
{
    if (! diagnostics::update::isEnabled (settings))
        return "Off: Flubsound does not look for updates.";
    const auto when = diagnostics::update::getLastCheckTime (settings);
    const auto text = diagnostics::update::getLastResultText (settings);
    if (when == juce::Time() || text.isEmpty())
        return "Not checked yet.";
    return "Last check " + when.formatted ("%Y-%m-%d %H:%M") + ": " + text;
}

juce::String SettingsDialog::exportDiagnostics (EngineController& controller, const juce::File& logFolder, const juce::File& zipFile,
                                                bool listDevices)
{
    diagnostics::BundleSources sources;
    sources.systemReport = diagnostics::describeEngine (controller, listDevices);
    sources.systemReport << "\nOutput device profile:\n" << describeOutputDevice (controller) << "\n";
    if (const auto streams = describeCaptureStreams (controller.getCaptureStreams()); streams.isNotEmpty())
        sources.systemReport << "\nPer-app capture streams:\n" << streams << "\n";
    sources.systemReport << "\nLoopback guard: " << describeLoopbackGuard (controller) << "\n";

    const auto settingsFile = controller.getSettings().getFile();
    sources.settingsFiles.add (settingsFile);
    sources.settingsFiles.add (settingsFile.getSiblingFile ("route-journal.json"));
    auto logs = logFolder.findChildFiles (juce::File::findFiles, false, "*.log");
    logs.sort();
    sources.logFiles = logs;
    sources.crashFolder = logFolder;
    return diagnostics::writeBundle (sources, zipFile);
}

void SettingsDialog::showPage (Page page)
{
    current = page;
    for (size_t i = 0; i < navButtons.size(); ++i)
        navButtons[i].setToggleState (static_cast<int> (i) == static_cast<int> (page), juce::dontSendNotification);
    audioView.setVisible (page == Page::Audio);
    correctionPage->setVisible (page == Page::Correction);
    processingView.setVisible (page == Page::Processing);
    routingView.setVisible (page == Page::Routing);
    hearingView.setVisible (page == Page::Hearing);
    hotkeysPage->setVisible (page == Page::Hotkeys);
    generalPage->setVisible (page == Page::General);
    diagnosticsPage->setVisible (page == Page::Diagnostics);
    if (page == Page::Processing)
        processingPage->refresh();
    if (page == Page::Routing)
        routingPage->refresh();
    if (page == Page::Correction)
        correctionPage->refresh();
    if (page == Page::Hearing)
        hearingPage->refresh();
    if (page == Page::Hotkeys)
        hotkeysPage->refresh();
    if (page == Page::General)
        generalPage->refresh();
    if (page == Page::Diagnostics)
        diagnosticsPage->refresh();
    repaint();
}

juce::String SettingsDialog::getHotkeysSummary() const
{
    return hotkeysPage->getSummary();
}

void SettingsDialog::timerCallback()
{
    if (current == Page::Processing)
        processingPage->refresh();
    if (current == Page::Routing)
        routingPage->refresh(); // the silent device and the moves follow the routing passes
    if (current == Page::Audio)
        audioPage->refresh();
    if (current == Page::Correction)
        correctionPage->refresh();
    if (current == Page::Hearing)
        hearingPage->refresh(); // the estimate and the dose are live
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
    // The Routing page too (one input-map row per strip).
    routingView.setBounds (pageArea.withTrimmedRight (-scrollbar));
    routingPage->setSize (pageArea.getWidth(), juce::jmax (1, routingPage->getHeight()));
    // The Hearing page too (docs/11 E32 (c) / E33: the per-ear editor below the guard).
    hearingView.setBounds (pageArea.withTrimmedRight (-scrollbar));
    hearingPage->setSize (pageArea.getWidth(), juce::jmax (1, hearingPage->getHeight()));
    correctionPage->setBounds (pageArea);
    hotkeysPage->setBounds (pageArea);
    generalPage->setBounds (pageArea);
    diagnosticsPage->setBounds (pageArea);
}
} // namespace flub::app::ui
