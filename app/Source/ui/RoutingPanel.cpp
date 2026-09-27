#include "RoutingPanel.h"

#include "FlubLookAndFeel.h"
#include "LevelMeters.h"
#include "Theme.h"
#include "platform/PlatformBridge.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
juce::String displayNameOf (const juce::String& executable)
{
    auto n = executable.trim().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
    if (n.endsWithIgnoreCase (".exe"))
        n = n.dropLastCharacters (4);
    return n;
}

juce::String channelBadge (int channels)
{
    if (channels >= 8)
        return "7.1";
    if (channels >= 6)
        return "5.1";
    return channels == 1 ? "MONO" : "STEREO";
}
} // namespace

// =============================================================================
// StripRow
// =============================================================================
class RoutingPanel::StripRow : public juce::Component, public juce::TooltipClient
{
public:
    /** An application routed to this strip. Its state is drawn as a shape as
        well as a colour, so it never relies on colour alone: accent dot with a
        halo (playing), plain dot (running, idle), hollow ring (not running),
        '!' badge in the palette's alert colour (routing / capture error). */
    struct Chip
    {
        enum class State
        {
            Playing,
            Idle,
            NotRunning,
            Error
        };
        juce::String name, executable, error;
        State state = State::NotRunning;
    };

    StripRow (RoutingPanel& p, int stripIndex)
        : panel (p), strip (stripIndex)
    {
        auto& ctrl = panel.controller;
        name = ctrl.getStripName (strip);
        channels = ctrl.getStripChannels (strip);
        setTitle (name + " strip");
        setDescription ("Click to edit the " + name + " strip");

        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        juce::NormalisableRange<double> range (-60.0, 12.0);
        range.setSkewForCentre (-12.0);
        gain.setNormalisableRange (range);
        gain.setDoubleClickReturnValue (true, 0.0);
        gain.setValue (ctrl.getStripGainDb (strip), juce::dontSendNotification);
        gain.textFromValueFunction = [] (double v) { return Theme::formatSignedDb (static_cast<float> (v), 1) + " dB"; };
        Style::describe (gain, name + " level", name + " strip level (double-click: 0 dB)");
        gain.onValueChange = [this] { panel.controller.setStripGainDb (strip, static_cast<float> (gain.getValue())); repaint (gainTextArea); };
        addAndMakeVisible (gain);

        mute.setClickingTogglesState (false);
        mute.setAccentWhenOn (false);
        mute.setIconColour (std::nullopt);
        mute.onClick = [this]
        {
            panel.controller.setStripMuted (strip, ! panel.controller.isStripMuted (strip));
            refreshState();
        };
        addAndMakeVisible (mute);
        refreshState();
    }

    int getStrip() const noexcept { return strip; }

    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    void setApps (std::vector<Chip> newChips)
    {
        chips = std::move (newChips);
        juce::StringArray states;
        for (const auto& chip : chips)
            states.add (describeChip (chip).replace ("\n", ": "));
        setDescription ("Click to edit the " + name + " strip" + (states.isEmpty() ? juce::String() : ". Apps: " + states.joinIntoString ("; ")));
        layoutChips();
        repaint();
    }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative();
        for (size_t i = 0; i < chips.size() && i < chipBounds.size(); ++i)
            if (chipBounds[i].contains (pos))
                return describeChip (chips[i]);
        if (hiddenChips > 0 && moreArea.contains (pos))
        {
            juce::StringArray hidden;
            for (size_t i = chips.size() - static_cast<size_t> (hiddenChips); i < chips.size(); ++i)
                hidden.add (describeChip (chips[i]));
            return hidden.joinIntoString ("\n");
        }
        return {};
    }

    void refreshState()
    {
        auto& ctrl = panel.controller;
        const bool muted = ctrl.isStripMuted (strip);
        mute.setIcon (muted ? Icons::speakerMuted() : Icons::speaker());
        mute.setIconColour (muted ? std::optional<juce::Colour> (Theme::statusColours (*this).hot) : std::nullopt);
        Style::describe (mute, muted ? "Unmute " + name : "Mute " + name, muted ? "Unmute this strip" : "Mute this strip");
        if (! gain.isMouseButtonDown())
            gain.setValue (ctrl.getStripGainDb (strip), juce::dontSendNotification);
        gain.setAlpha (muted ? 0.5f : 1.0f);
    }

    void updateMeters (float dt)
    {
        auto& ctrl = panel.controller;
        const bool isActive = ctrl.isStripActive (strip);
        const auto& bus = ctrl.getChain (strip).meters();
        if (isActive != active)
            repaint (ledArea.expanded (4));
        active = isActive;
        bool changed = false;
        for (size_t c = 0; c < 2; ++c)
        {
            const float target = active ? bus.outPeakDb[c].load (std::memory_order_relaxed) : -100.0f;
            const float next = juce::jmax (std::isfinite (target) ? target : -100.0f, levels[c] - 30.0f * dt);
            changed = changed || std::abs (next - levels[c]) > 0.2f;
            levels[c] = next;
        }
        if (changed)
            repaint (meterArea.expanded (2));
    }

    int getPreferredHeight (int width) const
    {
        return 80 + chipLines (width) * 22;
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto accent = Theme::accent (*this);
        g.setColour (selected ? accent.withAlpha (0.07f) : (isMouseOver (true) ? Palette::panelHover.withAlpha (0.5f) : Palette::panelRaised.withAlpha (0.35f)));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (selected ? accent.withAlpha (0.65f) : Palette::border);
        g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

        // Activity LED + name + channel badge
        g.setColour (active ? accent : Palette::borderStrong);
        g.fillEllipse (ledArea.toFloat());
        if (active)
        {
            g.setColour (accent.withAlpha (0.25f));
            g.fillEllipse (ledArea.toFloat().expanded (3.0f));
        }
        auto title = titleArea.toFloat();
        g.setColour (selected ? Palette::text : Palette::text.withAlpha (0.88f));
        g.setFont (Theme::font (13.5f, true));
        const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name);
        g.drawText (name, title.removeFromLeft (tw + 2.0f), juce::Justification::centredLeft, false);
        title.removeFromLeft (8.0f);
        const auto badge = channelBadge (channels);
        const float bw = juce::GlyphArrangement::getStringWidth (Theme::caption (9.0f), badge) + 12.0f;
        Theme::drawPill (g, title.removeFromLeft (bw).withSizeKeepingCentre (bw, 15.0f), badge, channels > 2 ? Palette::magenta : Palette::muted);

        // Gain value
        g.setColour (Palette::muted);
        g.setFont (Theme::numeric (11.5f, false));
        g.drawText (gain.getTextFromValue (gain.getValue()), gainTextArea, juce::Justification::centredRight, false);

        // Mini meter (stereo peak, IEC deflection -70 .. 0 dBFS)
        const auto colours = Theme::meterColours (*this);
        auto m = meterArea.toFloat();
        for (size_t c = 0; c < 2; ++c)
        {
            auto bar = m.removeFromTop (3.0f);
            m.removeFromTop (2.0f);
            g.setColour (Palette::well);
            g.fillRoundedRectangle (bar, 1.5f);
            const float level = LevelMeters::deflection (levels[c]);
            if (level > 0.0f)
            {
                g.setGradientFill (LevelMeters::gradient (colours, bar.getTopLeft(), bar.getTopRight()));
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * level), 1.5f);
            }
        }

        // App chips
        if (chips.empty())
        {
            g.setColour (Palette::faint);
            g.setFont (Theme::font (11.0f));
            g.drawText ("No apps assigned", chipsArea.toFloat(), juce::Justification::centredLeft, false);
        }
        const auto alert = Theme::statusColours (*this).hot; // red, or vermillion with the colour-blind palette
        for (size_t i = 0; i < chips.size() && i < chipBounds.size(); ++i)
        {
            const auto r = chipBounds[i].toFloat();
            if (r.isEmpty())
                continue;
            const auto state = chips[i].state;
            g.setColour (Palette::panelRaised);
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (state == Chip::State::Error ? alert.withAlpha (0.55f) : Palette::borderStrong);
            g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
            auto content = r.reduced (6.0f, 0.0f);
            drawChipState (g, content.removeFromLeft (11.0f), state, accent, alert);
            content.removeFromLeft (5.0f);
            g.setColour (Palette::text.withAlpha (state == Chip::State::NotRunning ? 0.6f : 0.9f));
            g.setFont (Theme::font (11.5f));
            g.drawText (chips[i].name, content, juce::Justification::centredLeft, true);
        }
        if (hiddenChips > 0)
        {
            g.setColour (Palette::muted);
            g.setFont (Theme::font (11.0f));
            g.drawText ("+" + juce::String (hiddenChips), moreArea.toFloat(), juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        auto top = r.removeFromTop (22);
        mute.setBounds (top.removeFromRight (24).withSizeKeepingCentre (24, 24));
        ledArea = top.removeFromLeft (8).withSizeKeepingCentre (8, 8);
        top.removeFromLeft (8);
        titleArea = top;

        r.removeFromTop (4);
        auto gainRow = r.removeFromTop (20);
        gainTextArea = gainRow.removeFromRight (62);
        gain.setBounds (gainRow.withTrimmedLeft (-4));
        r.removeFromTop (3);
        meterArea = r.removeFromTop (8).withTrimmedRight (62);
        r.removeFromTop (6);
        chipsArea = r;
        layoutChips();
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void lookAndFeelChanged() override { refreshState(); } // meter palette -> muted icon colour

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < chipBounds.size() && i < chips.size(); ++i)
        {
            if (chipBounds[i].contains (e.getPosition()))
            {
                panel.showChipMenu (chips[i].executable, chips[i].error);
                return;
            }
        }
        panel.controller.setSelectedStrip (strip);
    }

private:
    juce::String describeChip (const Chip& chip) const
    {
        switch (chip.state)
        {
            case Chip::State::Playing: return chip.name + ": playing (routed to " + name + ")";
            case Chip::State::Idle: return chip.name + ": running, not playing";
            case Chip::State::NotRunning: return chip.name + ": not running (routed to " + name + " when it starts)";
            case Chip::State::Error: return chip.name + ": routing error\n" + chip.error;
        }
        return chip.name;
    }

    static void drawChipState (juce::Graphics& g, juce::Rectangle<float> area, Chip::State state, juce::Colour accent, juce::Colour alert)
    {
        const auto c = area.getCentre();
        const auto circle = [c] (float diameter) { return juce::Rectangle<float> (diameter, diameter).withCentre (c); };
        switch (state)
        {
            case Chip::State::Playing: // like the strip activity LED
                g.setColour (accent.withAlpha (0.28f));
                g.fillEllipse (circle (10.0f));
                g.setColour (accent);
                g.fillEllipse (circle (6.0f));
                break;
            case Chip::State::Idle:
                g.setColour (Palette::muted);
                g.fillEllipse (circle (6.0f));
                break;
            case Chip::State::NotRunning:
                g.setColour (Palette::faint.brighter (0.35f));
                g.drawEllipse (circle (6.5f), 1.3f);
                break;
            case Chip::State::Error:
                g.setColour (alert);
                g.fillEllipse (circle (11.0f));
                g.setColour (Palette::well);
                g.fillRoundedRectangle (juce::Rectangle<float> (1.7f, 4.3f).withCentre (c.translated (0.0f, -1.25f)), 0.85f);
                g.fillEllipse (juce::Rectangle<float> (1.9f, 1.9f).withCentre (c.translated (0.0f, 2.75f)));
                break;
        }
    }

    int chipLines (int width) const
    {
        if (chips.empty())
            return 1;
        const int available = width - 20;
        int lines = 1, x = 0;
        for (const auto& chip : chips)
        {
            const int w = chipWidth (chip.name);
            if (x > 0 && x + w > available)
            {
                ++lines;
                x = 0;
            }
            x += w + 6;
        }
        return juce::jmin (lines, 3);
    }

    static int chipWidth (const juce::String& text)
    {
        return juce::jmin (150, static_cast<int> (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), text)) + 30); // 6 + 11 + 5 + text + 8
    }

    void layoutChips()
    {
        chipBounds.assign (chips.size(), {});
        hiddenChips = 0;
        int x = chipsArea.getX(), y = chipsArea.getY(), line = 0;
        const int maxLines = juce::jmax (1, (chipsArea.getHeight() + 4) / 22);
        for (size_t i = 0; i < chips.size(); ++i)
        {
            const int w = chipWidth (chips[i].name);
            if (x > chipsArea.getX() && x + w > chipsArea.getRight())
            {
                if (line + 1 >= maxLines)
                {
                    hiddenChips = static_cast<int> (chips.size() - i);
                    break;
                }
                ++line;
                x = chipsArea.getX();
                y += 22;
            }
            chipBounds[i] = { x, y, juce::jmin (w, chipsArea.getRight() - x), 18 };
            x += w + 6;
        }
        // "+N" goes after the last visible chip on the last line; a chip it
        // would overlap is hidden as well.
        for (auto i = chips.size() - static_cast<size_t> (hiddenChips); hiddenChips > 0 && i > 0 && x > chipsArea.getX() && x + 30 > chipsArea.getRight(); --i)
        {
            x = chipBounds[i - 1].getX();
            chipBounds[i - 1] = {};
            ++hiddenChips;
        }
        moreArea = { x, y, 30, 18 };
    }

    RoutingPanel& panel;
    const int strip;
    juce::String name;
    int channels = 2;
    juce::Slider gain;
    IconButton mute { "Mute", Icons::speaker() };
    bool selected = false, active = false;
    std::array<float, 2> levels { -100.0f, -100.0f };
    std::vector<Chip> chips;
    std::vector<juce::Rectangle<int>> chipBounds;
    int hiddenChips = 0;
    juce::Rectangle<int> ledArea, titleArea, gainTextArea, meterArea, chipsArea, moreArea;
};

// =============================================================================
// AutoProfileList - "while <app> is in front, <strip> plays <preset>"
// =============================================================================
class RoutingPanel::AutoProfileList : public juce::Component
{
public:
    explicit AutoProfileList (RoutingPanel& p)
        : panel (p)
    {
        setTitle ("Automatic profiles");

        enable.setButtonText ("Follow the app in front");
        Style::set (enable, "switch");
        Style::describe (enable, "Switch presets automatically", "Load each rule's preset while its application is in the foreground");
        enable.onClick = [this] { panel.controller.setAutoProfilesEnabled (enable.getToggleState()); };
        addAndMakeVisible (enable);

        addButton.setText ("Add automatic profile...");
        addButton.setTooltip ("Load a preset on a strip while an application is in the foreground");
        addButton.onClick = [this] { panel.showAddAutoProfileDialog(); };
        addAndMakeVisible (addButton);

        for (auto* label : { &emptyText, &statusText })
        {
            label->setFont (Theme::font (11.0f));
            label->setColour (juce::Label::textColourId, Palette::muted);
            label->setJustificationType (juce::Justification::topLeft);
            label->setMinimumHorizontalScale (1.0f);
            label->setBorderSize ({});
            addChildComponent (*label);
        }
        emptyText.setText ("No automatic profiles. Add one to switch a strip's preset while a game or app is in the foreground.",
                           juce::dontSendNotification);
        refresh();
    }

    /** "cs2 -> Game: Competitive FPS (Gaming), restores on exit". */
    static juce::String describeRule (const AutoProfileRule& rule, const PresetManager& presets)
    {
        const auto* preset = presets.findById (rule.presetId);
        const juce::String mode = rule.mode == AutoProfileRule::Mode::Music ? " (Music)" : (rule.mode == AutoProfileRule::Mode::Gaming ? " (Gaming)" : "");
        return displayNameOf (rule.executable) + " -> " + rule.stripName + ": " + (preset != nullptr ? preset->name : "missing preset " + rule.presetId)
               + mode + (rule.restoreOnExit ? ", restores on exit" : ", kept on exit");
    }

    void refresh()
    {
        auto& ctrl = panel.controller;
        const bool supported = ctrl.isAutoProfileSupported();
        enable.setToggleState (ctrl.getAutoProfilesEnabled(), juce::dontSendNotification);
        enable.setEnabled (supported);
        addButton.setEnabled (supported);

        const auto& rules = ctrl.getAutoProfileRules();
        const auto* active = ctrl.getActiveAutoProfile();
        rows.clear();
        for (size_t i = 0; i < rules.size(); ++i)
        {
            auto row = std::make_unique<RuleRow>();
            const auto& rule = rules[i];
            row->label.setText (describeRule (rule, ctrl.getPresetManager()), juce::dontSendNotification);
            row->label.setTooltip ("While " + rule.executable + " is in the foreground, the " + rule.stripName + " strip plays this preset"
                                   + (rule.restoreOnExit ? "; its previous preset returns when the application leaves."
                                                         : "; the preset stays when the application leaves."));
            row->label.setFont (Theme::font (11.5f, active != nullptr && *active == rule));
            row->label.setColour (juce::Label::textColourId, active != nullptr && *active == rule ? Theme::accent (*this) : Palette::text.withAlpha (0.88f));
            row->label.setMinimumHorizontalScale (1.0f);
            row->label.setBorderSize ({});
            addAndMakeVisible (row->label);

            row->remove = std::make_unique<IconButton> ("Remove the automatic profile for " + displayNameOf (rule.executable), Icons::close());
            row->remove->setTooltip ("Remove this automatic profile");
            juce::Component::SafePointer<RoutingPanel> safe (&panel);
            const auto removed = rule;
            // Deferred: removing rebuilds this list, which owns the button being clicked.
            row->remove->onClick = [safe, removed]
            {
                juce::MessageManager::callAsync ([safe, removed]
                                                 {
                                                     if (safe == nullptr)
                                                         return;
                                                     auto remaining = safe->controller.getAutoProfileRules();
                                                     remaining.erase (std::remove (remaining.begin(), remaining.end(), removed), remaining.end());
                                                     safe->controller.setAutoProfileRules (remaining);
                                                     safe->refreshRouting();
                                                 });
            };
            addAndMakeVisible (*row->remove);
            rows.push_back (std::move (row));
        }

        emptyText.setVisible (rules.empty());
        const auto status = supported ? ctrl.describeAutoProfile() : ctrl.getAutoProfileUnsupportedReason();
        statusText.setText (status, juce::dontSendNotification);
        statusText.setColour (juce::Label::textColourId, supported || status.isEmpty() ? Palette::muted : Palette::amber.withAlpha (0.9f));
        statusText.setVisible (status.isNotEmpty());
        resized();
        repaint();
    }

    int getPreferredHeight (int width) const
    {
        int h = kCaptionHeight + 6 + static_cast<int> (rows.size()) * kRowHeight;
        if (emptyText.isVisible())
            h += textHeight (emptyText.getText(), width) + 6;
        if (statusText.isVisible())
            h += textHeight (statusText.getText(), width) + 6;
        return h + 6 + kButtonHeight;
    }

    void paint (juce::Graphics& g) override
    {
        Theme::drawCaption (g, "AUTO PROFILES", getLocalBounds().removeFromTop (kCaptionHeight).toFloat());
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto caption = r.removeFromTop (kCaptionHeight);
        enable.setBounds (caption.removeFromRight (juce::jmin (180, caption.getWidth() / 2 + 40)));
        r.removeFromTop (6);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (kRowHeight);
            row->remove->setBounds (line.removeFromRight (22).withSizeKeepingCentre (22, 22));
            line.removeFromRight (4);
            row->label.setBounds (line);
        }
        for (auto* label : { &emptyText, &statusText })
        {
            if (! label->isVisible())
                continue;
            label->setBounds (r.removeFromTop (textHeight (label->getText(), getWidth())));
            r.removeFromTop (6);
        }
        r.removeFromTop (6);
        addButton.setBounds (r.removeFromTop (kButtonHeight));
    }

private:
    static constexpr int kCaptionHeight = 20, kRowHeight = 24, kButtonHeight = 30;

    struct RuleRow
    {
        juce::Label label;
        std::unique_ptr<IconButton> remove;
    };

    static int textHeight (const juce::String& text, int width)
    {
        juce::AttributedString s;
        s.append (text, Theme::font (11.0f), Palette::muted);
        juce::TextLayout layout;
        layout.createLayout (s, static_cast<float> (juce::jmax (40, width)));
        return juce::roundToInt (layout.getHeight()) + 2;
    }

    RoutingPanel& panel;
    juce::ToggleButton enable;
    IconButton addButton { "Add automatic profile...", Icons::plus(), IconButton::Style::Framed };
    juce::Label emptyText, statusText;
    std::vector<std::unique_ptr<RuleRow>> rows;
};

// =============================================================================
// RoutingPanel
// =============================================================================
RoutingPanel::RoutingPanel (EngineController& c)
    : controller (c)
{
    setTitle ("Strips and application routing");

    rowView.setViewedComponent (&rowHolder, false);
    rowView.setScrollBarsShown (true, false);
    rowView.setScrollBarThickness (8);
    addAndMakeVisible (rowView);

    assignButton.setText ("Assign app to strip...");
    assignButton.onClick = [this] { showAssignMenu(); };
    addAndMakeVisible (assignButton);

    systemButton.setText ("System sound settings");
    systemButton.onClick = [this] { controller.getRouting().openSystemRoutingSettings(); };
    addAndMakeVisible (systemButton);

    autoProfiles = std::make_unique<AutoProfileList> (*this);
    rowHolder.addAndMakeVisible (*autoProfiles);

    rebuildStrips();
}

RoutingPanel::~RoutingPanel()
{
    controller.getRouting().setLiveUpdates (false);
    rows.clear();
    autoProfiles.reset();
}

void RoutingPanel::rebuildStrips()
{
    rows.clear();
    for (int i = 0; i < controller.getNumStrips(); ++i)
    {
        auto row = std::make_unique<StripRow> (*this, i);
        rowHolder.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }
    selectedStrip = -1;
    setSelectedStrip (controller.getSelectedStrip());
    refreshRouting();
    resized();
}

void RoutingPanel::setSelectedStrip (int strip)
{
    selectedStrip = strip;
    for (auto& r : rows)
        r->setSelected (r->getStrip() == strip);
    if (strip >= 0 && strip < controller.getNumStrips())
        assignButton.setTooltip ("Route an application's audio to the " + controller.getStripName (strip) + " strip");
}

juce::String RoutingPanel::unsupportedReason() const
{
    auto& routing = controller.getRouting();
    if (routing.getEffectiveMethod() != AppRouting::Method::Disabled)
        return {};
    if (! platform_bridge::servicesCompiledIn())
        return "Per-app routing needs the Flubsound platform services, which are not part of this build. "
               "Send apps to a Flubsound output device in your system sound settings instead.";
    if (routing.getMethod() == AppRouting::Method::Disabled)
        return "Per-app routing is switched off (Settings > Processing).";
    return "This system supports neither per-app endpoint routing nor process capture. "
           "Choose a Flubsound output device per app in the system sound settings instead.";
}

void RoutingPanel::refreshRouting()
{
    auto& routing = controller.getRouting();
    const auto& apps = routing.getApps();

    for (auto& row : rows)
    {
        const auto stripName = controller.getStripName (row->getStrip());
        std::vector<StripRow::Chip> chips;
        for (const auto& route : routing.getRoutes())
        {
            if (! route.stripName.equalsIgnoreCase (stripName))
                continue;
            // Every running instance counts: an error wins, then playing, then idle.
            using State = StripRow::Chip::State;
            StripRow::Chip chip { displayNameOf (route.executable), route.executable, {}, State::NotRunning };
            for (const auto& app : apps)
            {
                if (! AppRouting::executablesMatch (app.executable, route.executable) || chip.state == State::Error)
                    continue;
                if (app.error.isNotEmpty())
                {
                    chip.state = State::Error;
                    chip.error = app.error;
                }
                else if (app.isActive)
                    chip.state = State::Playing;
                else if (chip.state == State::NotRunning)
                    chip.state = State::Idle;
            }
            chips.push_back (std::move (chip));
        }
        row->setApps (std::move (chips));
    }

    autoProfiles->refresh();

    reason = unsupportedReason();
    assignButton.setEnabled (reason.isEmpty());
    systemButton.setEnabled (routing.canEnumerateApps());
    systemButton.setTooltip (routing.canEnumerateApps() ? "Open the operating system's per-app audio device settings"
                                                        : "Not available on this system");
    resized();
    repaint();
}

void RoutingPanel::updateMeters (double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    for (auto& r : rows)
        r->updateMeters (dt);
}

void RoutingPanel::visibilityChanged()
{
    controller.getRouting().setLiveUpdates (isVisible());
}

void RoutingPanel::showAssignMenu()
{
    const int strip = juce::jlimit (0, juce::jmax (0, controller.getNumStrips() - 1), controller.getSelectedStrip());
    const auto stripName = controller.getStripName (strip);
    auto& routing = controller.getRouting();

    juce::PopupMenu menu;
    menu.addSectionHeader ("Route to " + stripName);

    // Running audio sessions, active ones first, one entry per executable.
    auto apps = routing.getApps();
    std::stable_sort (apps.begin(), apps.end(), [] (const auto& a, const auto& b) { return a.isActive && ! b.isActive; });
    juce::StringArray seen;
    std::vector<juce::String> executables;
    for (const auto& app : apps)
    {
        if (app.executable.isEmpty() || seen.contains (app.executable, true))
            continue;
        seen.add (app.executable);
        const auto current = routing.getStripNameForExecutable (app.executable);
        const auto label = (app.displayName.isNotEmpty() ? app.displayName : displayNameOf (app.executable))
                           + (app.isActive ? juce::String() : juce::String (" (idle)"));
        juce::PopupMenu::Item item (label);
        item.itemID = 100 + static_cast<int> (executables.size());
        item.isTicked = current.equalsIgnoreCase (stripName);
        item.shortcutKeyDescription = current.isNotEmpty() && ! item.isTicked ? current : juce::String();
        menu.addItem (item);
        executables.push_back (app.executable);
    }
    if (executables.empty())
        menu.addItem (1, routing.canEnumerateApps() ? "No applications are playing audio" : "Running applications cannot be listed here", false);
    menu.addSeparator();
    menu.addItem (2, "Type an executable name...");

    juce::Component::SafePointer<RoutingPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&assignButton),
                        [safe, executables, stripName] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == 2)
                            {
                                safe->promptForExecutable (stripName);
                                return;
                            }
                            const auto index = static_cast<size_t> (result - 100);
                            if (result >= 100 && index < executables.size())
                            {
                                safe->controller.getRouting().setRoute (executables[index], stripName);
                                safe->refreshRouting();
                            }
                        });
}

void RoutingPanel::promptForExecutable (const juce::String& stripName)
{
    auto* window = new juce::AlertWindow ("Assign an application",
                                          "Executable name of the application to route to the " + stripName + " strip (e.g. cs2.exe or Spotify).",
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("exe", {}, "Executable:");
    window->addButton ("Assign", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<RoutingPanel> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, stripName] (int result)
                                                                        {
                                                                            if (safe == nullptr || result != 1)
                                                                                return;
                                                                            const auto exe = window->getTextEditorContents ("exe").trim();
                                                                            if (exe.isNotEmpty())
                                                                            {
                                                                                safe->controller.getRouting().setRoute (exe, stripName);
                                                                                safe->refreshRouting();
                                                                            }
                                                                        }),
                             true);
}

void RoutingPanel::showChipMenu (const juce::String& executable, const juce::String& error)
{
    auto& routing = controller.getRouting();
    const auto current = routing.getStripNameForExecutable (executable);

    juce::PopupMenu move;
    for (int i = 0; i < controller.getNumStrips(); ++i)
    {
        const auto name = controller.getStripName (i);
        move.addItem (100 + i, name, ! name.equalsIgnoreCase (current), name.equalsIgnoreCase (current));
    }
    juce::PopupMenu menu;
    menu.addSectionHeader (displayNameOf (executable));
    if (error.isNotEmpty())
    {
        // Shortened here (menus do not wrap); the item opens the full text.
        juce::PopupMenu::Item item ("Routing error: " + (error.length() > 60 ? error.substring (0, 57).trimEnd() + "..." : error));
        item.itemID = 2;
        item.colour = Theme::statusColours (*this).hot;
        menu.addItem (item);
        menu.addSeparator();
    }
    menu.addSubMenu ("Move to strip", move);
    menu.addItem (1, "Remove from " + current);

    juce::Component::SafePointer<RoutingPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safe, executable, error] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == 2)
                            {
                                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                        displayNameOf (executable) + " - routing error", error, "OK", safe);
                                return;
                            }
                            auto& r = safe->controller.getRouting();
                            if (result == 1)
                                r.removeRoute (executable);
                            else if (result >= 100)
                                r.setRoute (executable, safe->controller.getStripName (result - 100));
                            safe->refreshRouting();
                        });
}

void RoutingPanel::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());

    auto h = headerArea.toFloat();
    Theme::drawCaption (g, "STRIPS & ROUTING", h);

    const auto method = controller.getRouting().getEffectiveMethod();
    const auto label = method == AppRouting::Method::EndpointRouting ? juce::String ("ENDPOINTS")
                                                                     : (method == AppRouting::Method::ProcessCapture ? juce::String ("CAPTURE")
                                                                                                                      : juce::String ("MANUAL"));
    const float w = juce::GlyphArrangement::getStringWidth (Theme::caption (9.0f), label) + 14.0f;
    Theme::drawPill (g, h.removeFromRight (w).withSizeKeepingCentre (w, 16.0f), label,
                     method == AppRouting::Method::Disabled ? Palette::muted : Theme::accent (*this), method != AppRouting::Method::Disabled);

    if (reason.isNotEmpty() && ! reasonArea.isEmpty())
    {
        auto r = reasonArea.toFloat();
        g.setColour (Palette::well.withAlpha (0.7f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        if (reasonCompact)
        {
            auto line = r.reduced (9.0f, 0.0f);
            auto icon = line.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 14.0f);
            g.setColour (Palette::amber.withAlpha (0.9f));
            g.drawEllipse (icon.reduced (1.0f), 1.3f);
            g.setFont (Theme::font (10.0f, true));
            g.drawText ("i", icon, juce::Justification::centred, false);
            line.removeFromLeft (7.0f);
            g.setColour (Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawText ("Per-app routing unavailable - why?", line, juce::Justification::centredLeft, true);
        }
        else
        {
            layoutReason (reasonArea.getWidth()).draw (g, r.reduced (9.0f, 6.0f));
        }
    }
}

void RoutingPanel::mouseMove (const juce::MouseEvent& e)
{
    setTooltip (reasonCompact && reasonArea.contains (e.getPosition()) ? reason : juce::String());
}

void RoutingPanel::mouseUp (const juce::MouseEvent& e)
{
    if (reasonCompact && reasonArea.contains (e.getPosition()))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Per-app routing", reason, "OK", this);
}

juce::TextLayout RoutingPanel::layoutReason (int width) const
{
    juce::AttributedString text;
    text.append (reason, Theme::font (11.0f), Palette::muted);
    text.setLineSpacing (1.5f);
    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (juce::jmax (40, width - 18)));
    return layout;
}

void RoutingPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    headerArea = r.removeFromTop (20);
    r.removeFromTop (8);

    // Footer: the two actions stacked, the explanation (if any) above them.
    systemButton.setBounds (r.removeFromBottom (30));
    r.removeFromBottom (6);
    assignButton.setBounds (r.removeFromBottom (30));
    r.removeFromBottom (8);

    int rowsHeight = autoProfiles->getPreferredHeight (r.getWidth()) + 8;
    for (auto& row : rows)
        rowsHeight += row->getPreferredHeight (r.getWidth()) + 8;

    reasonArea = {};
    reasonCompact = false;
    if (reason.isNotEmpty())
    {
        const int fullHeight = juce::roundToInt (layoutReason (r.getWidth()).getHeight()) + 13;
        reasonCompact = rowsHeight + fullHeight + 8 > r.getHeight();
        reasonArea = r.removeFromBottom (reasonCompact ? 26 : fullHeight);
        r.removeFromBottom (8);
    }

    rowView.setBounds (r);
    const int width = r.getWidth() - (rowView.isVerticalScrollBarShown() ? rowView.getScrollBarThickness() + 2 : 0);
    int y = 0;
    for (auto& row : rows)
    {
        const int h = row->getPreferredHeight (width);
        row->setBounds (0, y, width, h);
        y += h + 8;
    }
    y += 4;
    const int autoHeight = autoProfiles->getPreferredHeight (width);
    autoProfiles->setBounds (0, y, width, autoHeight);
    y += autoHeight + 8;
    rowHolder.setSize (width, juce::jmax (0, y - 8));
}
} // namespace flub::app::ui
