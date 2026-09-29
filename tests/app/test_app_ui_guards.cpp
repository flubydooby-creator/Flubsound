// App-level tests: the Dynamic Range (guard.range, docs/11 E21: the Startle
// Guard and the Gaming Tame band) and Smoothness (smooth.amount, docs/11
// E07) controls.
// * The Simple view: a row under the macros in the Boost panel (Simple
//   layout only), inside the panel, clear of the knobs and the chips, bound
//   to the selected strip.
// * The Advanced view: Dynamic Range on the Compressor card and Smoothness
//   on the Clarity card of the module rack; both act with their card's
//   module off, so they are not dimmed with it.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "ui/BoostPanel.h"
#include "ui/MainComponent.h"
#include "ui/ModuleCard.h"
#include "ui/ModuleRack.h"

#include "flub/engine/Parameters.h"

#include <memory>

using namespace flub::app;
using namespace flub::param;
using View = AppSettings::MainView;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

template <typename T>
T* findByTitle (juce::Component& root, const juce::String& title)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child); t != nullptr && t->getTitle() == title)
            return t;
        if (auto* found = findByTitle<T> (*child, title))
            return found;
    }
    return nullptr;
}

/** `c` and every parent up to `root` are visible (no peer needed). */
bool visibleIn (juce::Component& root, juce::Component& c)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible())
            return false;
    return true;
}

ui::ModuleCard* findCard (juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (auto* card = dynamic_cast<ui::ModuleCard*> (child); card != nullptr && card->getDescriptor().id == id)
            return card;
        if (auto* found = findCard (*child, id))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE ("App UI: the Simple view has Dynamic Range and Smoothness under the macros, bound to the selected strip (E21 / E07)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::MainComponent main (controller);
    auto& boost = main.getBoostPanel();
    auto& range = boost.getDynamicRangeBox();
    auto& smooth = boost.getSmoothnessSlider();

    for (const auto& [w, h] : { std::pair { 1100, 700 }, std::pair { 1280, 820 }, std::pair { 2560, 1440 } })
    {
        main.setView (View::Simple, false);
        main.setSize (w, h);
        REQUIRE (range.isVisible());
        REQUIRE (smooth.isVisible());
        const auto panel = boost.getLocalBounds();
        CHECK (panel.contains (range.getBounds()));
        CHECK (panel.contains (smooth.getBounds()));
        CHECK (range.getWidth() >= 120);
        CHECK (smooth.getWidth() >= 120);
        CHECK (! range.getBounds().intersects (smooth.getBounds()));
        for (auto* child : boost.getChildren())
            if (auto* knob = dynamic_cast<ui::ParamKnob*> (child))
            {
                CHECK (! knob->getBounds().intersects (range.getBounds()));
                CHECK (! knob->getBounds().intersects (smooth.getBounds()));
            }
        CHECK (boost.getChipRowCount() >= 1);

        // The Advanced view's Boost strip has no room for them: the rack has.
        main.setView (View::Advanced, false);
        CHECK (! range.isVisible());
        CHECK (! smooth.isVisible());
    }
    main.setView (View::Simple, false);
    main.setSize (1280, 820);
    CHECK (boost.getChipRowCount() == 3); // the row costs the Simple view no chip row at the default size

    // Off / 0 % by default; they write the selected strip.
    auto& store = controller.getSelectedParams();
    CHECK (range.getText() == "Off");
    CHECK (smooth.getValue() == 0.0);
    CHECK (range.getNumItems() == 5);
    range.setSelectedItemIndex (3, juce::sendNotificationSync);
    CHECK (store.get (GuardRange) == static_cast<float> (GuardRangeValue::Lu10Balanced));
    smooth.setValue (0.5, juce::sendNotificationSync);
    CHECK (store.get (SmoothAmount) == 0.5f);
    CHECK (range.getTooltip().startsWith ("Dynamic Range:"));
    CHECK (smooth.getTooltip().startsWith ("Smoothness:"));
}

TEST_CASE ("App UI: the rack has Dynamic Range on the Compressor card and Smoothness on the Clarity card, not dimmed with the module (E21 / E07)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::ModuleRack rack (controller);
    rack.setSize (2400, 200); // every card in view
    rack.updateFromEngine();

    auto* comp = findCard (rack, "comp");
    auto* clarity = findCard (rack, "clarity");
    REQUIRE (comp != nullptr);
    REQUIRE (clarity != nullptr);
    auto* range = findByTitle<juce::ComboBox> (*comp, "Dynamic Range");
    auto* smooth = findByTitle<juce::Slider> (*clarity, "Smoothness");
    REQUIRE (range != nullptr);
    REQUIRE (smooth != nullptr);
    CHECK (visibleIn (rack, *range));
    CHECK (visibleIn (rack, *smooth));
    CHECK (range->getTooltip().contains ("works with Compressor switched off too"));
    CHECK (smooth->getTooltip().contains ("works with Clarity switched off too"));
    CHECK (range->getWidth() >= 100); // wide enough for "10 LU (Balanced)"
    CHECK (comp->getLocalBounds().contains (comp->getLocalArea (range, range->getLocalBounds())));

    // Both modules are off by default: their own controls dim, these do not.
    auto* ratio = findByTitle<juce::Slider> (*comp, "Ratio");
    REQUIRE (ratio != nullptr);
    CHECK (ratio->getParentComponent()->getAlpha() < 0.5f);
    CHECK (range->getAlpha() == 1.0f);
    CHECK (smooth->getParentComponent()->getAlpha() == 1.0f);

    // Bound to the selected strip.
    auto& store = controller.getSelectedParams();
    range->setSelectedItemIndex (4, juce::sendNotificationSync);
    CHECK (store.get (GuardRange) == static_cast<float> (GuardRangeValue::Lu6Shield));
    smooth->setValue (0.25, juce::sendNotificationSync);
    CHECK (store.get (SmoothAmount) == 0.25f);
}
