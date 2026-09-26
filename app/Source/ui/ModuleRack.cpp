#include "ModuleRack.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

ModuleRack::ModuleRack (EngineController& c)
    : controller (c),
      binder ([this] { return &controller.getSelectedParams(); },
              [this] { return &controller.getChain (controller.getSelectedStrip()); })
{
    setTitle ("Module rack");
    setWantsKeyboardFocus (false);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (10);
    viewport.getHorizontalScrollBar().addListener (this);
    addAndMakeVisible (viewport);

    for (const auto& d : ModuleDescriptor::all())
    {
        auto card = std::make_unique<ModuleCard> (d, binder);
        card->onExpandRequested = [this] (ModuleCard& which, bool shouldExpand) { expand (which, shouldExpand); };
        card->onListen = [this, enableId = d.enableId] (bool listen)
        {
            if (listen)
                listenStrip = controller.getSelectedStrip();
            controller.setAuditionBypass (listenStrip, enableId, listen);
        };
        if (d.banding == ModuleDescriptor::Banding::Eq)
            card->onBandChanged = [this] (int band)
            {
                if (onEqBandSelected != nullptr)
                    onEqBandSelected (band);
            };
        content.addAndMakeVisible (*card);
        cards.push_back (std::move (card));
    }
}

ModuleRack::~ModuleRack()
{
    viewport.getHorizontalScrollBar().removeListener (this);
    cards.clear(); // unbinds from the binder while it is still alive
}

void ModuleRack::updateFromEngine()
{
    auto& store = controller.getSelectedParams();
    const int strip = controller.getSelectedStrip();
    const auto& chain = controller.getChain (strip);
    const bool quality = static_cast<int> (std::lround (store.get (LatencyProfile))) == static_cast<int> (LatencyProfileValue::Quality);
    const bool surround = controller.getStripChannels (strip) > 2;

    for (auto& card : cards)
    {
        const int enableId = card->getDescriptor().enableId;
        ModuleCard::State s;
        s.baseOn = store.get (enableId) >= 0.5f;
        s.effectiveOn = chain.effectiveValue (enableId) >= 0.5f;
        s.gateInactiveProfile = ! quality;
        s.virtualizerNeedsSurround = ! surround;
        card->setState (s);
    }
}

void ModuleRack::releaseListening()
{
    for (auto& card : cards)
        card->stopListening();
}

void ModuleRack::setSelectedEqBand (int band)
{
    if (band < 0)
        return;
    for (auto& card : cards)
        if (card->getDescriptor().banding == ModuleDescriptor::Banding::Eq)
            card->setBand (band);
}

void ModuleRack::expand (ModuleCard& card, bool shouldExpand)
{
    if (shouldExpand)
    {
        if (expandedCard != nullptr && expandedCard != &card)
            expandedCard->setExpanded (false);
        expandedCard = &card;
        card.setExpanded (true);
    }
    else
    {
        card.setExpanded (false);
        if (expandedCard == &card)
            expandedCard = nullptr;
    }

    for (auto& c : cards)
        c->setVisible (expandedCard == nullptr || c.get() == expandedCard);
    viewport.setScrollBarsShown (false, expandedCard == nullptr);
    resized();
    if (onLayoutModeChanged != nullptr)
        onLayoutModeChanged();
}

void ModuleRack::collapse()
{
    if (expandedCard != nullptr)
        expand (*expandedCard, false);
}

bool ModuleRack::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && expandedCard != nullptr)
    {
        collapse();
        return true;
    }
    return false;
}

void ModuleRack::paint (juce::Graphics&)
{
}

void ModuleRack::paintOverChildren (juce::Graphics& g)
{
    // Edge fades hint at cards scrolled out of view.
    if (expandedCard != nullptr || content.getWidth() <= viewport.getWidth())
        return;
    const float fade = 36.0f;
    const auto area = viewport.getBounds().toFloat().withTrimmedBottom (static_cast<float> (viewport.getScrollBarThickness()) + 2.0f);
    const int x = viewport.getViewPositionX();
    if (x > 0)
    {
        g.setGradientFill (juce::ColourGradient (Palette::background, area.getX(), 0.0f, Palette::background.withAlpha (0.0f), area.getX() + fade, 0.0f, false));
        g.fillRect (area.withWidth (fade));
    }
    if (x + viewport.getWidth() < content.getWidth())
    {
        g.setGradientFill (juce::ColourGradient (Palette::background.withAlpha (0.0f), area.getRight() - fade, 0.0f, Palette::background, area.getRight(), 0.0f,
                                                 false));
        g.fillRect (area.withLeft (area.getRight() - fade));
    }
}

void ModuleRack::resized()
{
    viewport.setBounds (getLocalBounds());

    if (expandedCard != nullptr)
    {
        content.setBounds (0, 0, viewport.getWidth(), viewport.getHeight());
        expandedCard->setBounds (content.getLocalBounds());
        viewport.setViewPosition (0, 0);
        return;
    }

    // Row of cards; the scrollbar sits under them when they do not fit.
    int total = 0;
    for (auto& c : cards)
        total += c->getPreferredWidth() + 10;
    total -= 10;

    const bool fits = total <= viewport.getWidth();
    const int height = viewport.getHeight() - (fits ? 0 : viewport.getScrollBarThickness() + 4);
    // When everything fits, spread the spare width over the cards.
    const float stretch = fits && total > 0 ? static_cast<float> (viewport.getWidth() + 10 - static_cast<int> (cards.size()) * 10)
                                                  / static_cast<float> (total + 10 - static_cast<int> (cards.size()) * 10)
                                            : 1.0f;
    int x = 0;
    for (auto& c : cards)
    {
        const int w = juce::roundToInt (static_cast<float> (c->getPreferredWidth()) * stretch);
        c->setBounds (x, 0, w, height);
        x += w + 10;
    }
    content.setBounds (0, 0, juce::jmax (viewport.getWidth(), x - 10), height);
}
} // namespace flub::app::ui
