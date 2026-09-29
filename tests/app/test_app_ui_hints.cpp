// App-level tests: the plain-language hints (docs/11 E39).
//
// * ParamHints: every key of the parameter layout has a hint in both modes,
//   at most ParamHints::kMaxLength (140) characters, one sentence or two,
//   never the bare display name; the macros and Boost have their own Gaming
//   text; banded keys name their band.
// * Where they show: the module cards' key controls and power switches, an
//   expanded card's grid and the Boost panel's dial and macros (whose hint
//   follows the mode).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "ui/BoostPanel.h"
#include "ui/ModuleCard.h"
#include "ui/ModuleRack.h"
#include "ui/ParamHints.h"

#include "flub/engine/Parameters.h"

#include <iostream>

using namespace flub::app;
using namespace flub::param;

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

void collectSliders (juce::Component& c, std::vector<juce::Slider*>& out)
{
    for (auto* child : c.getChildren())
    {
        if (auto* s = dynamic_cast<juce::Slider*> (child))
            out.push_back (s);
        collectSliders (*child, out);
    }
}
} // namespace

TEST_CASE ("App UI: every parameter key has a plain-language hint of at most 140 characters in both modes (E39)")
{
    namespace H = ui::ParamHints;
    int keys = 0, modeSpecific = 0;
    size_t longest = 0;
    for (int id = 0; id < kNumParams; ++id)
    {
        const auto& info = layout()[static_cast<size_t> (id)];
        const juce::String key (info.key);
        for (const auto mode : { ModeValue::Music, ModeValue::Gaming })
        {
            const auto hint = H::get (id, mode);
            if (hint.isEmpty())
                std::cerr << "    no hint for " << key << "\n";
            REQUIRE (hint.isNotEmpty());
            if (hint.length() > H::kMaxLength)
                std::cerr << "    " << key << ": " << hint.length() << " characters\n";
            CHECK (hint.length() <= H::kMaxLength);
            CHECK (hint != juce::String (info.name));
            CHECK (! hint.containsChar ('#'));                                    // the band placeholder is filled in
            CHECK ((hint.endsWithChar ('.') || hint.endsWithChar (')')));          // a sentence
            longest = std::max (longest, static_cast<size_t> (hint.length()));
        }
        modeSpecific += H::differsByMode (key) ? 1 : 0;
        ++keys;
    }
    std::cerr << "    " << keys << " keys, " << modeSpecific << " with a Gaming text of their own, longest hint " << longest << " characters\n";
    CHECK (keys == kNumParams);

    // The macros and Boost mean different things per mode.
    for (const int id : { static_cast<int> (Macro1), static_cast<int> (Macro2), static_cast<int> (Macro3), static_cast<int> (Macro4),
                          static_cast<int> (Macro5), static_cast<int> (BoostIntensity) })
    {
        CHECK (H::get (id, ModeValue::Music) != H::get (id, ModeValue::Gaming));
        CHECK (H::differsByMode (juce::String (layout()[static_cast<size_t> (id)].key)));
    }
    CHECK (H::get (Macro1, ModeValue::Music).startsWith ("Punch"));
    CHECK (H::get (Macro1, ModeValue::Gaming).startsWith ("Footsteps"));
    CHECK (H::get (BassBoostDb, ModeValue::Music) == H::get (BassBoostDb, ModeValue::Gaming));

    // Banded keys name their band; unknown keys and ids have none.
    CHECK (H::get (eq (3, EqFieldGain), ModeValue::Music).contains ("band 4"));
    CHECK (H::get (dyn (1, DynFieldThreshold), ModeValue::Music).contains ("band 2"));
    CHECK (H::forKey ("no.such.key", ModeValue::Music).isEmpty());
    CHECK (H::get (-1, ModeValue::Music).isEmpty());
    CHECK (H::get (kNumParams, ModeValue::Music).isEmpty());
    CHECK (H::tooltip (BassHarmonics, ModeValue::Music).startsWith ("Harmonic Bass: Bass you can hear on small speakers"));
}

TEST_CASE ("App UI: module cards, their expanded grids and the Boost panel show the hints as tooltips; the macros' follow the mode (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));

    // Module cards: key controls and power switches.
    ui::ModuleRack rack (controller);
    rack.setSize (1400, 200);
    rack.updateFromEngine();
    int checkedKeys = 0;
    for (auto* card : rack.getShownCards())
    {
        std::vector<juce::Slider*> sliders;
        collectSliders (*card, sliders);
        for (auto* s : sliders)
        {
            CHECK (s->getTooltip().contains (": "));
            CHECK (s->getTooltip().length() > s->getTitle().length() + 20);
            ++checkedKeys;
        }
    }
    CHECK (checkedKeys >= 30);

    // An expanded card's grid: every parameter of the module.
    for (auto* card : rack.getShownCards())
    {
        if (card->getDescriptor().id != "bass")
            continue;
        card->setExpanded (true);
        std::vector<juce::Slider*> sliders;
        collectSliders (*card, sliders);
        bool sawCutoff = false;
        for (auto* s : sliders)
        {
            CHECK (s->getTooltip().isNotEmpty());
            sawCutoff = sawCutoff || s->getTooltip().startsWith ("Speaker Low Limit: The lowest note your speakers can play");
        }
        CHECK (sawCutoff);
        card->setExpanded (false);
    }

    // The Boost panel: dial and macros, per mode.
    ui::BoostPanel boost (controller);
    boost.setSize (900, 212);
    boost.setMode (ModeValue::Music);
    std::vector<juce::Slider*> sliders;
    collectSliders (boost, sliders);
    auto tipOf = [&sliders] (const juce::String& title)
    {
        for (auto* s : sliders)
            if (s->getTitle() == title)
                return s->getTooltip();
        return juce::String();
    };
    CHECK (tipOf ("Punch").startsWith ("Punch: sharper drum hits"));
    CHECK (tipOf ("Boost Intensity").contains ("clarity and width first"));
    boost.setMode (ModeValue::Gaming);
    CHECK (tipOf ("Footsteps").startsWith ("Footsteps: lifts steps and movement"));
    CHECK (tipOf ("Boost Intensity").contains ("detail and direction first"));
}
