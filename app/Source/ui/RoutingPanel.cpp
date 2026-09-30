#include "RoutingPanel.h"

#include "FlubLookAndFeel.h"
#include "LevelMeters.h"
#include "Theme.h"
#if JUCE_LINUX
 #include "platform/pipewire/PipeWireGraph.h"
#endif

#include <cmath>
#include <iterator>
#include <utility>

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

constexpr int kFormRowHeight = 30, kFormLabelWidth = 90; // AutoProfileForm

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
            Doubled, // docs/11 E47: held back by the doubling guard, its original is heard directly
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

        // The Chat strip's voice-chat line (docs/11 E22): ChatMix, the duck
        // switch and its depth, and the voice dot.
        isChat = name.equalsIgnoreCase ("Chat");
        if (isChat)
        {
            chatMix.setSliderStyle (juce::Slider::LinearHorizontal);
            chatMix.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            chatMix.setRange (-1.0, 1.0, 0.1);
            chatMix.setDoubleClickReturnValue (true, 0.0);
            chatMix.onValueChange = [this]
            {
                panel.controller.setChatMix (static_cast<float> (chatMix.getValue()));
                refreshChat();
            };
            addAndMakeVisible (chatMix);

            Style::set (duck, "switch");
            duck.setButtonText (kDuckText);
            duck.onClick = [this] { panel.controller.setChatDuck (duck.getToggleState()); refreshChat(); };
            addAndMakeVisible (duck);

            duckDepth.setSliderStyle (juce::Slider::LinearHorizontal);
            duckDepth.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
            duckDepth.setRange (EngineController::kMinChatDuckDepthDb, EngineController::kMaxChatDuckDepthDb, 0.5);
            duckDepth.setDoubleClickReturnValue (true, EngineController::kDefaultChatDuckDepthDb);
            duckDepth.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
            duckDepth.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue(); };
            duckDepth.onValueChange = [this]
            {
                panel.controller.setChatDuck (panel.controller.getChatDuck(), static_cast<float> (duckDepth.getValue()));
                refreshChat();
            };
            Style::describe (duckDepth, "Duck depth", "How far the game and music dip at 1 - 4 kHz while a teammate talks (3 - 6 dB; double-click: 4.5 dB)");
            addAndMakeVisible (duckDepth);
            refreshChat();
        }
        refreshState();
    }

    int getStrip() const noexcept { return strip; }
    bool isChatRow() const noexcept { return isChat; }
    juce::Slider& getChatMixSlider() noexcept { return chatMix; }
    juce::Button& getDuckButton() noexcept { return duck; }
    juce::Slider& getDuckDepthSlider() noexcept { return duckDepth; }
    bool isVoiceLit() const noexcept { return voiceLit; }

    /** The Chat line from the controller (a hotkey moved ChatMix, a setting changed). */
    void refreshChat()
    {
        if (! isChat)
            return;
        auto& ctrl = panel.controller;
        const bool possible = ctrl.hasChatMix();
        if (! chatMix.isMouseButtonDown())
            chatMix.setValue (ctrl.getChatMix(), juce::dontSendNotification);
        chatMix.setEnabled (possible);
        Style::describe (chatMix, "ChatMix",
                         possible ? "Balance between the Game and the Chat strip: " + ctrl.describeChatMix() + " (double-click: centre)"
                                  : juce::String ("ChatMix needs a Game and a Chat strip"));
        duck.setToggleState (ctrl.getChatDuck(), juce::dontSendNotification);
        Style::describe (duck, "Duck game under voice chat",
                         "While a teammate talks on this strip, the Game and Music strips dip at 1 - 4 kHz so the voice stays clear "
                         "(footsteps untouched)");
        if (! duckDepth.isMouseButtonDown())
            duckDepth.setValue (ctrl.getChatDuckDepthDb(), juce::dontSendNotification);
        duckDepth.setEnabled (ctrl.getChatDuck());
        repaint (voiceArea.expanded (4));
    }

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

        if (isChat)
        {
            const bool lit = ctrl.hasChatMix() && ctrl.isChatVoiceActive();
            if (lit != voiceLit)
            {
                voiceLit = lit;
                repaint (voiceArea.expanded (4));
            }
            if (! chatMix.isMouseButtonDown() && std::abs (chatMix.getValue() - static_cast<double> (ctrl.getChatMix())) > 0.01)
                refreshChat(); // the ChatMix hotkeys
        }
    }

    int getPreferredHeight (int width) const
    {
        return 80 + chipLines (width) * 22 + (isChat ? kChatLinesHeight + (chatOnTwoLines (width - 20) ? 26 : 0) : 0);
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

        // The voice dot (Chat row): lit with a halo while the strip carries
        // speech, a ring otherwise (shape as well as colour).
        if (isChat)
        {
            auto v = voiceArea.toFloat();
            const auto led = v.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f);
            if (voiceLit)
            {
                g.setColour (accent.withAlpha (0.25f));
                g.fillEllipse (led.expanded (3.0f));
                g.setColour (accent);
                g.fillEllipse (led);
            }
            else
            {
                g.setColour (Palette::borderStrong);
                g.drawEllipse (led.reduced (0.5f), 1.3f);
            }
            if (v.getWidth() > 30.0f)
            {
                g.setColour (voiceLit ? Palette::text : Palette::muted);
                g.setFont (Theme::font (11.0f));
                g.drawText (voiceLit ? "Voice" : "Quiet", v.withTrimmedLeft (4.0f), juce::Justification::centredLeft, false);
            }
            Theme::drawCaption (g, "GAME", gameCaption.toFloat());
            Theme::drawCaption (g, "CHAT", chatCaption.toFloat());
        }

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
        const auto caution = Theme::statusColours (*this).warn; // amber (the colour-blind palette's own)
        for (size_t i = 0; i < chips.size() && i < chipBounds.size(); ++i)
        {
            const auto r = chipBounds[i].toFloat();
            if (r.isEmpty())
                continue;
            const auto state = chips[i].state;
            g.setColour (Palette::panelRaised);
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (state == Chip::State::Error ? alert.withAlpha (0.55f)
                                                     : (state == Chip::State::Doubled ? caution.withAlpha (0.6f) : Palette::borderStrong));
            g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
            auto content = r.reduced (6.0f, 0.0f);
            drawChipState (g, content.removeFromLeft (11.0f), state, accent, alert, caution);
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
        if (isChat)
        {
            // ChatMix and the voice dot (with its word where there is room);
            // the duck switch and its depth (on two lines in a narrow column).
            const bool twoLines = chatOnTwoLines (r.getWidth());
            auto line = r.removeFromTop (22);
            gameCaption = line.removeFromLeft (38);
            voiceArea = line.removeFromRight (twoLines ? 14 : 56);
            line.removeFromRight (4);
            chatCaption = line.removeFromRight (34);
            chatMix.setBounds (line);
            r.removeFromTop (4);
            line = r.removeFromTop (24);
            const bool fullText = juce::GlyphArrangement::getStringWidth (Theme::font (13.0f), kDuckText) + 40.0f
                                  <= static_cast<float> (twoLines ? line.getWidth() : kDuckWidth);
            duck.setButtonText (fullText ? kDuckText : "Duck game under chat");
            if (twoLines)
            {
                duck.setBounds (line);
                r.removeFromTop (2);
                line = r.removeFromTop (24);
            }
            else
            {
                duck.setBounds (line.removeFromLeft (kDuckWidth));
                line.removeFromLeft (8);
            }
            duckDepth.setBounds (line);
            r.removeFromTop (6);
        }
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
                panel.showChipMenu (chips[i].executable, chips[i].error, chips[i].state == Chip::State::Doubled);
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
            case Chip::State::Doubled:
                return chip.name + ": original also audible (plays straight to the output device, so it is not captured; click for the fix)";
            case Chip::State::Error: return chip.name + ": routing error\n" + chip.error;
        }
        return chip.name;
    }

    static void drawChipState (juce::Graphics& g, juce::Rectangle<float> area, Chip::State state, juce::Colour accent, juce::Colour alert,
                               juce::Colour caution)
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
            case Chip::State::Doubled: // two overlapping rings: heard twice
                g.setColour (caution);
                g.drawEllipse (circle (6.5f).translated (-1.8f, 0.0f), 1.3f);
                g.drawEllipse (circle (6.5f).translated (1.8f, 0.0f), 1.3f);
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

    // The Chat row's voice-chat line (docs/11 E22).
    static constexpr int kChatLinesHeight = 22 + 4 + 24 + 6, kDuckWidth = 220;
    static constexpr const char* kDuckText = "Duck game under voice chat";
    /** The duck's depth goes under the switch below this row width. */
    static bool chatOnTwoLines (int innerWidth) noexcept { return innerWidth < kDuckWidth + 8 + 140; }
    bool isChat = false, voiceLit = false;
    juce::Slider chatMix, duckDepth;
    juce::ToggleButton duck;
    juce::Rectangle<int> gameCaption, chatCaption, voiceArea;
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

        enable.setButtonText ({}); // the caption labels it; the title names it for screen readers
        Style::set (enable, "switch");
        Style::describe (enable, "Follow the app in front", "Automatic profiles on / off: load each rule's preset while its application is in the foreground");
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
        // The switch alone (no text) at the caption's right end: with its
        // text it would cover the caption in the narrow panel.
        auto caption = r.removeFromTop (kCaptionHeight);
        enable.setBounds (caption.removeFromRight (kSwitchWidth));
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
    static constexpr int kCaptionHeight = 20, kSwitchWidth = 36, kRowHeight = 24, kButtonHeight = 30;

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
// AutoProfileForm - the fields of "Add automatic profile..."
// =============================================================================
AutoProfileForm::AutoProfileForm (EngineController& c)
    : controller (c)
{
    setTitle ("New automatic profile");

    // Applications seen in the foreground (newest first), then the ones
    // playing audio; one entry per executable.
    const auto offer = [this] (const juce::String& exe)
    {
        if (exe.trim().isEmpty())
            return;
        for (const auto& known : suggestions)
            if (AppRouting::executablesMatch (known, exe))
                return;
        suggestions.add (exe.trim());
    };
    for (const auto& exe : controller.getRecentForegroundApps())
        offer (exe);
    for (const auto& app : controller.getRouting().getApps())
        offer (app.executable);

    application.setText (suggestions.isEmpty() ? juce::String() : suggestions[0], false);
    application.setTextToShowWhenEmpty ("e.g. cs2.exe, Spotify or com.spotify.client", Palette::muted);
    Style::describe (application, "Application", "Executable name (with or without .exe), full path, or macOS bundle id");
    addAndMakeVisible (application);

    for (int i = 0; i < suggestions.size(); ++i)
        recent.addItem (displayNameOf (suggestions[i]), i + 1);
    recent.setTextWhenNothingSelected (suggestions.isEmpty() ? "No applications seen yet" : "Pick a recent application...");
    recent.setTextWhenNoChoicesAvailable ("No applications seen yet");
    recent.setEnabled (! suggestions.isEmpty());
    recent.onChange = [this]
    {
        const int index = recent.getSelectedId() - 1;
        if (index >= 0 && index < suggestions.size())
            application.setText (suggestions[index], false);
    };
    Style::describe (recent, "Recent applications", "Applications recently in the foreground or playing audio");
    addAndMakeVisible (recent);

    const int selected = juce::jlimit (0, juce::jmax (0, controller.getNumStrips() - 1), controller.getSelectedStrip());
    for (int i = 0; i < controller.getNumStrips(); ++i)
        strip.addItem (controller.getStripName (i), i + 1);
    strip.setSelectedId (selected + 1, juce::dontSendNotification);
    Style::describe (strip, "Strip", "The strip whose preset changes");
    addAndMakeVisible (strip);

    // Presets grouped like the preset menu (factory by category, then user).
    const auto current = controller.getCurrentPresetId (selected);
    juce::String heading;
    for (const auto& p : controller.getPresetManager().getPresets())
    {
        const auto group = (p.isFactory ? juce::String() : juce::String ("User: ")) + p.category;
        if (group != heading)
        {
            heading = group;
            preset.addSectionHeading (group.isNotEmpty() ? group : juce::String ("User"));
        }
        presetIds.push_back (p.id);
        preset.addItem (p.name, static_cast<int> (presetIds.size()));
        if (p.id == current)
            preset.setSelectedId (static_cast<int> (presetIds.size()), juce::dontSendNotification);
    }
    if (preset.getSelectedId() == 0 && ! presetIds.empty())
        preset.setSelectedId (1, juce::dontSendNotification);
    Style::describe (preset, "Preset", "The preset the strip plays while the application is in the foreground");
    addAndMakeVisible (preset);

    mode.addItem ("The preset's own mode", 1);
    mode.addItem ("Music", 2);
    mode.addItem ("Gaming", 3);
    mode.setSelectedId (1, juce::dontSendNotification);
    Style::describe (mode, "Mode", "Play the preset in its own mode, or force Music or Gaming mode");
    addAndMakeVisible (mode);

    Style::describe (restore, "Restore on exit",
                     "On: the strip's previous preset (and unsaved edits) return when the application leaves the foreground. "
                     "Off: the preset stays.");
    addAndMakeVisible (restore);

    const std::pair<juce::Label*, juce::Component*> labels[] = { { &applicationLabel, &application }, { &recentLabel, &recent },
                                                                 { &stripLabel, &strip }, { &presetLabel, &preset }, { &modeLabel, &mode } };
    const char* texts[] = { "Application", "Recent", "Strip", "Preset", "Mode" };
    for (size_t i = 0; i < std::size (labels); ++i)
    {
        labels[i].first->setText (texts[i], juce::dontSendNotification);
        labels[i].first->setFont (Theme::font (12.0f));
        labels[i].first->setColour (juce::Label::textColourId, Palette::muted);
        labels[i].first->attachToComponent (labels[i].second, true);
    }
    setSize (400, 6 * kFormRowHeight);
}

AutoProfileRule AutoProfileForm::getRule() const
{
    AutoProfileRule rule;
    rule.executable = application.getText().trim();
    rule.stripName = strip.getText();
    const int index = preset.getSelectedId() - 1;
    if (index >= 0 && index < static_cast<int> (presetIds.size()))
        rule.presetId = presetIds[static_cast<size_t> (index)];
    rule.mode = mode.getSelectedId() == 2 ? AutoProfileRule::Mode::Music
                                          : (mode.getSelectedId() == 3 ? AutoProfileRule::Mode::Gaming : AutoProfileRule::Mode::Preset);
    rule.restoreOnExit = restore.getToggleState();
    return rule;
}

void AutoProfileForm::resized()
{
    auto r = getLocalBounds();
    r.removeFromLeft (kFormLabelWidth); // the attached labels sit here
    for (auto* field : std::initializer_list<juce::Component*> { &application, &recent, &strip, &preset, &mode })
        field->setBounds (r.removeFromTop (kFormRowHeight).reduced (0, 3));
    restore.setBounds (r.removeFromTop (kFormRowHeight));
}

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

void RoutingPanel::refreshRouting()
{
    auto& routing = controller.getRouting();
    const auto& apps = routing.getApps();
    bool anyAssignedRunning = false, anyAssignedFailing = false, anyAssignedDoubled = false;

    for (auto& row : rows)
    {
        const auto stripName = controller.getStripName (row->getStrip());
        std::vector<StripRow::Chip> chips;
        for (const auto& route : routing.getRoutes())
        {
            if (! route.stripName.equalsIgnoreCase (stripName))
                continue;
            // Every running instance counts: an error wins, then the doubling
            // guard (docs/11 E47), then playing, then idle.
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
                else if (app.doublingBlocked || chip.state == State::Doubled)
                    chip.state = State::Doubled;
                else if (app.isActive)
                    chip.state = State::Playing;
                else if (chip.state == State::NotRunning)
                    chip.state = State::Idle;
            }
            anyAssignedRunning = anyAssignedRunning || chip.state == State::Playing || chip.state == State::Idle;
            anyAssignedFailing = anyAssignedFailing || chip.state == State::Error;
            anyAssignedDoubled = anyAssignedDoubled || chip.state == State::Doubled;
            chips.push_back (std::move (chip));
        }
        row->setApps (std::move (chips));
    }

    autoProfiles->refresh();
    refreshLinkStatus();

    // The red state: no application reaches a strip through us, whatever the
    // reason (an unavailable method gives its own). Not while nothing is
    // assigned and the device input feeds a strip (a virtual cable set as the
    // system output): the audio is processed then, just not per app, and the
    // notice stays the neutral explanation.
    reason = routing.getUnavailableReason();
    const bool fedByDeviceInput = routing.getRoutes().empty() && controller.getDeviceInputStrip() >= 0;
    noAppsProcessed = routing.getProcessedAppCount() == 0 && ! fedByDeviceInput;
    const auto doublingText = anyAssignedDoubled ? routing.describeDoubling() : juce::String();
    doubling = ! noAppsProcessed && doublingText.isNotEmpty();
    if (! noAppsProcessed)
        noticeDetail = doubling ? doublingText : juce::String();
    else if (reason.isNotEmpty())
        noticeDetail = reason;
    else if (doublingText.isNotEmpty())
        noticeDetail = doublingText;
    else if (routing.getRoutes().empty())
        noticeDetail = "No application is assigned to a strip yet. Use \"Assign app to strip...\" below.";
    else if (anyAssignedFailing)
        noticeDetail = "The assigned applications could not be routed. Click an application marked '!' for the reason.";
    else if (anyAssignedRunning)
        noticeDetail = "The assigned applications are not routed yet.";
    else
        noticeDetail = "None of the assigned applications is running. Each one is routed when it starts.";
    notice = noAppsProcessed ? "No apps are being processed. " + noticeDetail : (doubling ? "Original also audible. " + noticeDetail : reason);
    setDescription (notice);

    assignButton.setEnabled (reason.isEmpty());
    systemButton.setEnabled (routing.canEnumerateApps());
    systemButton.setTooltip (routing.canEnumerateApps() ? "Open the operating system's per-app audio device settings"
                                                        : "Not available on this system");
    resized();
    repaint();
}

RoutingPanel::StripRow* RoutingPanel::findChatRow() const
{
    for (const auto& r : rows)
        if (r->isChatRow())
            return r.get();
    return nullptr;
}

juce::Slider* RoutingPanel::getChatMixSlider()
{
    auto* row = findChatRow();
    return row != nullptr ? &row->getChatMixSlider() : nullptr;
}

juce::Button* RoutingPanel::getChatDuckButton()
{
    auto* row = findChatRow();
    return row != nullptr ? &row->getDuckButton() : nullptr;
}

juce::Slider* RoutingPanel::getChatDuckDepthSlider()
{
    auto* row = findChatRow();
    return row != nullptr ? &row->getDuckDepthSlider() : nullptr;
}

bool RoutingPanel::isVoiceDotLit() const
{
    const auto* row = findChatRow();
    return row != nullptr && row->isVoiceLit();
}

void RoutingPanel::refreshChat()
{
    if (auto* row = findChatRow())
        row->refreshChat();
}

void RoutingPanel::updateMeters (double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    for (auto& r : rows)
        r->updateMeters (dt);

    // The links and the quantum change on their own (a sink appears, another
    // client lowers the quantum): re-read twice a second.
    linkPollSeconds += juce::jlimit (0.0, 0.25, dtSeconds);
    if (linkPollSeconds >= 0.5)
    {
        linkPollSeconds = 0.0;
        refreshLinkStatus();
    }
}

void RoutingPanel::refreshLinkStatus()
{
    juce::String text;
    bool warning = false;
   #if JUCE_LINUX
    const auto node = controller.getHost().getNativeNodeStatus();
    if (node.running)
    {
        text = "PipeWire: " + juce::String (flub::platform::pipewire::describeLinks (node));
        warning = ! node.message.empty() || node.inputLinksMade < node.inputLinksWanted || node.outputLinksMade < node.outputLinksWanted
                  || node.outputSink.empty();
    }
    else
   #endif
    {
        auto& routing = controller.getRouting();
        text = routing.getInputLinkStatus();
        warning = ! routing.areInputsLinked();
    }
    if (text == linkStatus && warning == linkWarning)
        return;
    const bool relayout = text.isEmpty() != linkStatus.isEmpty();
    linkStatus = text;
    linkWarning = warning;
    if (relayout)
        resized();
    repaint (linkArea);
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

void RoutingPanel::showAddAutoProfileDialog()
{
    if (! controller.isAutoProfileSupported())
        return;

    // The form outlives the window: the modal callback owns it, and the
    // window (deleted right after the callback) only detaches it.
    auto form = std::make_shared<AutoProfileForm> (controller);
    auto* window = new juce::AlertWindow ("Add automatic profile",
                                          "While the application is in the foreground, the strip plays the preset.",
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addCustomComponent (form.get());
    window->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    // Return in the application field adds too (the editor consumes the key).
    form->application.onReturnKey = [w = juce::Component::SafePointer<juce::AlertWindow> (window)]
    {
        if (w != nullptr)
            w->exitModalState (1);
    };

    juce::Component::SafePointer<RoutingPanel> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, form] (int result)
                                                                        {
                                                                            if (safe == nullptr || result != 1)
                                                                                return;
                                                                            if (! safe->controller.addAutoProfileRule (form->getRule()))
                                                                                juce::AlertWindow::showMessageBoxAsync (
                                                                                    juce::MessageBoxIconType::WarningIcon, "Add automatic profile",
                                                                                    "Enter the application's executable name and choose a preset.", "OK",
                                                                                    safe.getComponent());
                                                                            safe->refreshRouting();
                                                                        }),
                             true);
}

void RoutingPanel::showDoublingFix()
{
    auto& routing = controller.getRouting();
    const auto text = routing.describeDoubling();
    if (text.isEmpty())
        return;
    auto* window = new juce::AlertWindow ("Original also audible", text, juce::MessageBoxIconType::WarningIcon, this);
    window->addButton ("Open sound settings", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RoutingPanel> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
                                                                        {
                                                                            if (safe != nullptr && result == 1)
                                                                                safe->controller.getRouting().openSystemRoutingSettings();
                                                                        }),
                             true);
}

juce::String RoutingPanel::getStripDescription (int strip) const
{
    for (const auto& row : rows)
        if (row->getStrip() == strip)
            return row->getDescription();
    return {};
}

void RoutingPanel::showChipMenu (const juce::String& executable, const juce::String& error, bool doubled)
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
    if (doubled)
    {
        juce::PopupMenu::Item item ("Original also audible - how to fix...");
        item.itemID = 3;
        item.colour = Theme::statusColours (*this).warn;
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
                            if (result == 3)
                            {
                                safe->showDoublingFix();
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

    if (notice.isNotEmpty() && ! noticeArea.isEmpty())
    {
        // Red state: alert colour (vermillion with the colour-blind palette)
        // plus a '!' shape and a title, so it never relies on colour alone.
        // Amber (doubling guard): the caution colour, a hollow '!' and a title.
        const auto alert = Theme::statusColours (*this).hot;
        const auto caution = Theme::statusColours (*this).warn;
        auto r = noticeArea.toFloat();
        g.setColour (noAppsProcessed ? alert.withAlpha (0.1f) : (doubling ? caution.withAlpha (0.1f) : Palette::well.withAlpha (0.7f)));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (noAppsProcessed ? alert.withAlpha (0.75f) : (doubling ? caution.withAlpha (0.75f) : Palette::border));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        if (noticeCompact)
        {
            auto line = r.reduced (9.0f, 0.0f);
            auto icon = line.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 14.0f);
            g.setColour (noAppsProcessed ? alert : (doubling ? caution : Palette::amber.withAlpha (0.9f)));
            if (noAppsProcessed)
                g.fillEllipse (icon.reduced (0.5f));
            else
                g.drawEllipse (icon.reduced (1.0f), 1.3f);
            g.setFont (Theme::font (10.0f, true));
            if (noAppsProcessed)
                g.setColour (Palette::well);
            g.drawText (noAppsProcessed || doubling ? "!" : "i", icon, juce::Justification::centred, false);
            line.removeFromLeft (7.0f);
            g.setColour (noAppsProcessed ? alert : (doubling ? caution : Palette::muted));
            g.setFont (Theme::font (11.5f, noAppsProcessed || doubling));
            juce::String text (noAppsProcessed ? "No apps are being processed - why?"
                                               : (doubling ? "Original also audible - fix?" : "Per-app routing unavailable - why?"));
            if (noAppsProcessed && juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) > line.getWidth())
                text = "No apps processed - why?"; // the narrow panel
            g.drawText (text, line, juce::Justification::centredLeft, true);
        }
        else
        {
            layoutNotice (noticeArea.getWidth()).draw (g, r.reduced (9.0f, 6.0f));
        }
    }

    if (linkStatus.isNotEmpty() && ! linkArea.isEmpty())
    {
        g.setColour (linkWarning ? Theme::statusColours (*this).warn : Palette::muted);
        g.setFont (Theme::font (10.5f));
        g.drawFittedText (linkStatus, linkArea, juce::Justification::centredLeft, 1, 1.0f); // elided; the full text is the tooltip
    }
}

void RoutingPanel::mouseMove (const juce::MouseEvent& e)
{
    if (linkArea.contains (e.getPosition()))
        setTooltip (linkStatus);
    else
        setTooltip (noticeCompact && noticeArea.contains (e.getPosition()) ? notice : juce::String());
}

void RoutingPanel::mouseUp (const juce::MouseEvent& e)
{
    if (doubling && noticeArea.contains (e.getPosition()))
        showDoublingFix();
    else if (noticeCompact && noticeArea.contains (e.getPosition()))
        juce::AlertWindow::showMessageBoxAsync (noAppsProcessed ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon,
                                                noAppsProcessed ? "No apps are being processed" : "Per-app routing",
                                                noAppsProcessed ? noticeDetail : notice, "OK", this);
}

juce::TextLayout RoutingPanel::layoutNotice (int width) const
{
    juce::AttributedString text;
    if (noAppsProcessed)
    {
        text.append ("No apps are being processed\n", Theme::font (12.0f, true), Theme::statusColours (*this).hot);
        text.append (noticeDetail, Theme::font (11.0f), Palette::muted);
    }
    else if (doubling)
    {
        text.append ("Original also audible\n", Theme::font (12.0f, true), Theme::statusColours (*this).warn);
        text.append (noticeDetail, Theme::font (11.0f), Palette::muted);
    }
    else
    {
        text.append (notice, Theme::font (11.0f), Palette::muted);
    }
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

    // Footer: the two actions stacked. The notice (if any) goes under the header.
    systemButton.setBounds (r.removeFromBottom (30));
    r.removeFromBottom (6);
    assignButton.setBounds (r.removeFromBottom (30));
    r.removeFromBottom (8);
    linkArea = {};
    if (linkStatus.isNotEmpty())
    {
        linkArea = r.removeFromBottom (16);
        r.removeFromBottom (6);
    }

    int rowsHeight = autoProfiles->getPreferredHeight (r.getWidth()) + 8;
    for (auto& row : rows)
        rowsHeight += row->getPreferredHeight (r.getWidth()) + 8;

    noticeArea = {};
    noticeCompact = false;
    if (notice.isNotEmpty())
    {
        const int fullHeight = juce::roundToInt (layoutNotice (r.getWidth()).getHeight()) + 13;
        noticeCompact = rowsHeight + fullHeight + 8 > r.getHeight();
        noticeArea = r.removeFromTop (noticeCompact ? 26 : fullHeight);
        r.removeFromTop (8);
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
