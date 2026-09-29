#include "QuickControls.h"

#include "ParamHints.h"
#include "Theme.h"

namespace flub::app::ui
{
using namespace flub::param;

QuickControls::QuickControls (EngineController& c)
    : controller (c),
      binder ([this] { return &controller.getSelectedParams(); },
              [this] { return &controller.getChain (controller.getSelectedStrip()); })
{
    setTitle ("Flubsound Pro quick controls");

    boost.setSliderStyle (juce::Slider::LinearHorizontal);
    boost.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
    boost.setTitle ("Boost Intensity");
    binder.bindSlider (boost, BoostIntensity);
    addAndMakeVisible (boost);

    previous.onClick = [this] { controller.previousPreset(); };
    next.onClick = [this] { controller.nextPreset(); };
    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    addAndMakeVisible (previous);
    addAndMakeVisible (next);

    Style::set (bypass, "warning");
    bypass.onClick = [this] { controller.toggleEnabled(); };
    addAndMakeVisible (bypass);

    open.setTooltip ("Open the Flubsound Pro window");
    open.onClick = [this]
    {
        if (onOpenWindow != nullptr)
            onOpenWindow();
    };
    addAndMakeVisible (open);

    // ChatMix (docs/11 E22): the controller's balance, in its 10 % steps.
    chatMix.setSliderStyle (juce::Slider::LinearHorizontal);
    chatMix.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    chatMix.setRange (-1.0, 1.0, 0.1);
    chatMix.setDoubleClickReturnValue (true, 0.0);
    chatMix.onValueChange = [this]
    {
        controller.setChatMix (static_cast<float> (chatMix.getValue()));
        refresh();
    };
    addAndMakeVisible (chatMix);

    controller.addListener (this);
    refresh();
    pollVoice();
    startTimerHz (kVoicePollHz);
    setSize (kWidth, kHeight);
}

QuickControls::~QuickControls()
{
    stopTimer();
    controller.removeListener (this);
    binder.unbind (boost);
}

void QuickControls::show (EngineController& c, juce::Rectangle<int> screenArea, std::function<void()> openWindow)
{
    auto content = std::make_unique<QuickControls> (c);
    auto* raw = content.get();
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (content), screenArea, nullptr);
    juce::Component::SafePointer<juce::CallOutBox> safeBox (&box);
    raw->onOpenWindow = [safeBox, openWindow]
    {
        if (safeBox != nullptr)
            safeBox->dismiss();
        if (openWindow != nullptr)
            openWindow();
    };
}

void QuickControls::engineControllerChanged (EngineController::Change change)
{
    using Change = EngineController::Change;
    if (change == Change::Preset || change == Change::MasterEnable || change == Change::SelectedStrip || change == Change::Parameters
        || change == Change::Engine)
        refresh();
}

void QuickControls::pollVoice()
{
    const bool lit = controller.hasChatMix() && controller.isChatVoiceActive();
    if (lit == voiceLit)
        return;
    voiceLit = lit;
    repaint (voiceArea.expanded (2));
}

void QuickControls::refresh()
{
    const auto name = controller.getCurrentPresetName();
    presetName = name.isNotEmpty() ? name : juce::String ("Default settings");
    if (controller.isPresetModified())
        presetName << " *";
    const bool bypassed = ! controller.isEnabled();
    bypass.setToggleState (bypassed, juce::dontSendNotification);
    bypass.setButtonText (bypassed ? "Bypassed" : "Bypass");
    Style::describe (bypass, "Bypass all processing", bypassed ? "Flubsound is bypassed: click to switch the processing back on"
                                                               : "Hear every strip without processing (loudness matched while that option is on)");
    boost.setTooltip (ParamHints::tooltip (BoostIntensity, controller.getMode()));
    const bool hasPresets = ! controller.getPresetManager().getPresets().empty();
    previous.setEnabled (hasPresets);
    next.setEnabled (hasPresets);

    const bool chat = controller.hasChatMix();
    if (! chatMix.isMouseButtonDown())
        chatMix.setValue (controller.getChatMix(), juce::dontSendNotification);
    chatMix.setEnabled (chat);
    Style::describe (chatMix, "ChatMix",
                     chat ? "Balance between the Game and the Chat strip: " + controller.describeChatMix() + " (double-click: centre)"
                          : juce::String ("ChatMix needs a Game and a Chat strip"));
    repaint();
}

void QuickControls::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::text);
    g.setFont (Theme::font (13.5f, true));
    g.drawText ("Flubsound Pro  -  " + controller.getStripName (controller.getSelectedStrip()) + " strip", titleArea,
                juce::Justification::centredLeft, true);
    Theme::drawCaption (g, "BOOST", boostCaption.toFloat());

    auto p = presetArea.toFloat();
    g.setColour (Palette::well);
    g.fillRoundedRectangle (p, Theme::kControlRadius);
    g.setColour (Palette::border);
    g.drawRoundedRectangle (p.reduced (0.5f), Theme::kControlRadius, 1.0f);
    g.setColour (Palette::text);
    g.setFont (Theme::font (13.0f));
    g.drawText (presetName, presetArea.reduced (8, 0), juce::Justification::centred, true);

    // ChatMix captions and the voice dot (lit: accent with a halo, like a
    // strip's activity LED; idle: a ring, so it never relies on colour).
    Theme::drawCaption (g, "GAME", gameCaption.toFloat());
    Theme::drawCaption (g, "CHAT", chatCaption.toFloat());
    const auto accent = Theme::accent (*this);
    auto dot = voiceArea.toFloat();
    const auto led = dot.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f);
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
    g.setColour (voiceLit ? Palette::text : Palette::muted);
    g.setFont (Theme::font (11.5f));
    g.drawText (voiceLit ? "Voice" : "Quiet", dot.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
}

void QuickControls::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (28);
    open.setBounds (top.removeFromRight (28));
    top.removeFromRight (8);
    titleArea = top;
    r.removeFromTop (8);

    auto row = r.removeFromTop (30);
    boostCaption = row.removeFromLeft (52);
    boost.setBounds (row);
    r.removeFromTop (10);

    row = r.removeFromTop (30);
    previous.setBounds (row.removeFromLeft (30));
    next.setBounds (row.removeFromRight (30));
    row.reduce (6, 0);
    presetArea = row;
    r.removeFromTop (10);

    bypass.setBounds (r.removeFromTop (30).withWidth (110));
    r.removeFromTop (10);

    row = r.removeFromTop (30);
    gameCaption = row.removeFromLeft (40);
    voiceArea = row.removeFromRight (58);
    row.removeFromRight (6);
    chatCaption = row.removeFromRight (36);
    chatMix.setBounds (row);
}
} // namespace flub::app::ui
