// App-level tests of the tray (R4.4, docs/06 §7.1): the quick menu, built
// without a tray icon (TrayIcon::buildMenu, the same code the icon's right
// click shows), each item acting on the selected strip or calling its
// callback, the "hotkeys not active" item, and the icon images. No tray icon
// is created: a test must not put an icon in the user's notification area,
// and CI runners have no tray host.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "shell/TrayIcon.h"

#include <vector>

using namespace flub::app;

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
    return o;
}

std::vector<juce::PopupMenu::Item> itemsOf (const juce::PopupMenu& menu)
{
    std::vector<juce::PopupMenu::Item> items;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.push_back (it.getItem());
    return items;
}

/** The item with this text (or starting with it), nullptr if none. */
const juce::PopupMenu::Item* itemNamed (const std::vector<juce::PopupMenu::Item>& items, const juce::String& text)
{
    for (const auto& item : items)
        if (item.text == text || (text.endsWith ("*") && item.text.startsWith (text.dropLastCharacters (1))))
            return &item;
    return nullptr;
}

juce::StringArray textsOf (const std::vector<juce::PopupMenu::Item>& items)
{
    juce::StringArray texts;
    for (const auto& item : items)
        if (! item.isSeparator)
            texts.add (item.text);
    return texts;
}
} // namespace

TEST_CASE ("App: the tray menu has every R4.4 item and each acts on the selected strip or calls back (enable, mode, Boost, presets, Tournament, hotkeys not active, quick controls, open, quit)")
{
    using flub::param::ModeValue;
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    int opened = 0, quits = 0, quick = 0, hotkeySettings = 0, problems = 0;
    TrayIcon::Callbacks callbacks;
    callbacks.openWindow = [&opened] { ++opened; };
    callbacks.quit = [&quits] { ++quits; };
    callbacks.openHotkeySettings = [&hotkeySettings] { ++hotkeySettings; };
    callbacks.countHotkeyProblems = [&problems] { return problems; };
    const auto build = [&] { return itemsOf (TrayIcon::buildMenu (controller, callbacks, [&quick] { ++quick; })); };

    const int music = controller.findStrip ("Music"), game = controller.findStrip ("Game");
    REQUIRE (music >= 0);
    REQUIRE (game >= 0);
    controller.setSelectedStrip (music);
    controller.setMode (ModeValue::Music, music);
    controller.setBoost (0.5f, music);
    controller.setBoost (0.3f, game);

    auto items = build();
    REQUIRE (! items.empty());
    CHECK (items[0].isSectionHeader);
    CHECK (textsOf (items)
           == (juce::StringArray { "Flubsound Pro - Music", "Enabled", "Music Mode", "Gaming Mode", "Boost +10%   (now 50%)", "Boost -10%", "Presets",
                                   "Tournament mode", "Tournament mode when an anti-cheat runs", "Quick controls...", "Open Flubsound Pro", "Quit" }));

    // Enabled: ticked while processing; choosing it switches every strip off and on.
    const auto* enabled = itemNamed (items, "Enabled");
    REQUIRE (enabled != nullptr);
    CHECK (enabled->isTicked == controller.isEnabled());
    const bool wasEnabled = controller.isEnabled();
    enabled->action();
    CHECK (controller.isEnabled() != wasEnabled);
    CHECK (itemNamed (build(), "Enabled")->isTicked == controller.isEnabled());
    controller.setEnabled (true);

    // Mode on the selected strip only.
    CHECK (itemNamed (items, "Music Mode")->isTicked);
    CHECK (! itemNamed (items, "Gaming Mode")->isTicked);
    const auto gameMode = controller.getMode (game);
    itemNamed (items, "Gaming Mode")->action();
    CHECK (controller.getMode (music) == ModeValue::Gaming);
    CHECK (controller.getMode (game) == gameMode);
    items = build();
    CHECK (itemNamed (items, "Gaming Mode")->isTicked);
    itemNamed (items, "Music Mode")->action();
    CHECK (controller.getMode (music) == ModeValue::Music);

    // Boost +10 % / -10 % on the selected strip, greyed at the ends.
    items = build();
    itemNamed (items, "Boost +10%*")->action();
    CHECK (controller.getBoost (music) == 0.6f);
    CHECK (controller.getBoost (game) == 0.3f);
    itemNamed (items, "Boost -10%")->action();
    CHECK (controller.getBoost (music) == 0.5f);
    controller.setBoost (1.0f, music);
    items = build();
    CHECK (itemNamed (items, "Boost +10%*")->text == "Boost +10%   (now 100%)");
    CHECK (! itemNamed (items, "Boost +10%*")->isEnabled);
    CHECK (itemNamed (items, "Boost -10%")->isEnabled);
    controller.setBoost (0.0f, music);
    items = build();
    CHECK (itemNamed (items, "Boost +10%*")->isEnabled);
    CHECK (! itemNamed (items, "Boost -10%")->isEnabled);

    // Presets: the factory list by category; choosing one loads it on the selected strip, ticked afterwards.
    const auto* presets = itemNamed (items, "Presets");
    REQUIRE (presets != nullptr);
    REQUIRE (presets->subMenu != nullptr);
    const auto categories = itemsOf (*presets->subMenu);
    REQUIRE (! categories.empty());
    const auto* category = &categories.front();
    REQUIRE (category->subMenu != nullptr); // several categories: one submenu each
    const auto first = itemsOf (*category->subMenu);
    REQUIRE (! first.empty());
    REQUIRE (first.front().action != nullptr);
    CHECK (! first.front().isTicked);
    first.front().action();
    CHECK (controller.getCurrentPresetName (music) == first.front().text);
    CHECK (controller.getCurrentPresetId (game).isEmpty());
    const auto again = itemsOf (*itemsOf (*itemNamed (build(), "Presets")->subMenu).front().subMenu);
    CHECK (again.front().isTicked);
    size_t total = 0;
    for (const auto& c : categories)
        total += itemsOf (*c.subMenu).size();
    CHECK (total == controller.getPresetManager().getFactoryPresets().size());

    // Tournament mode (docs/11 E55): the switch and the automatic switch-on.
    REQUIRE (itemNamed (items, "Tournament mode") != nullptr);
    itemNamed (items, "Tournament mode")->action();
    CHECK (controller.isTournamentActive());
    CHECK (itemNamed (build(), "Tournament mode")->isTicked);
    itemNamed (build(), "Tournament mode")->action();
    CHECK (! controller.isTournamentActive());
    REQUIRE (itemNamed (items, "Tournament mode when an anti-cheat runs") != nullptr);

    // Hotkeys that failed to register: one item, only then, opening Settings > Hotkeys.
    CHECK (! textsOf (items).joinIntoString ("|").contains ("not active"));
    problems = 2;
    items = build();
    const auto* fix = itemNamed (items, "2 hotkeys not active - fix...");
    REQUIRE (fix != nullptr);
    CHECK (fix->isEnabled);
    fix->action();
    CHECK (hotkeySettings == 1);
    problems = 1;
    CHECK (itemNamed (build(), "1 hotkey not active - fix...") != nullptr);
    callbacks.openHotkeySettings = nullptr;
    CHECK (! itemNamed (build(), "1 hotkey not active - fix...")->isEnabled);

    // The window, the flyout and quitting.
    items = build();
    itemNamed (items, "Quick controls...")->action();
    itemNamed (items, "Open Flubsound Pro")->action();
    itemNamed (items, "Quit")->action();
    CHECK (quick == 1);
    CHECK (opened == 1);
    CHECK (quits == 1);

    // The header names the selected strip.
    controller.setSelectedStrip (game);
    CHECK (build().front().text == "Flubsound Pro - Game");
    controller.shutdown();
}

TEST_CASE ("App: the tray icon is drawn in code: a bright tile while enabled, a grey one while bypassed, black bars alone for the macOS template")
{
    const auto on = TrayIcon::createIconImage (true, false, 32);
    const auto off = TrayIcon::createIconImage (false, false, 32);
    const auto mask = TrayIcon::createIconImage (true, true, 32);
    REQUIRE (on.getWidth() == 32);
    REQUIRE (on.getHeight() == 32);

    // Above the bars, inside the tile: the gradient's colour.
    const auto onTile = on.getPixelAt (16, 3), offTile = off.getPixelAt (16, 3);
    CHECK (onTile.getAlpha() == 255);
    CHECK (offTile.getAlpha() == 255);
    CHECK (onTile.getSaturation() > 0.5f);  // teal to blue
    CHECK (offTile.getSaturation() < 0.25f); // grey
    CHECK (on.getPixelAt (0, 0).getAlpha() < 64); // rounded corner

    // The middle bar is white (dimmed while bypassed); the template has no tile.
    CHECK (on.getPixelAt (16, 16).getBrightness() > 0.95f);
    CHECK (off.getPixelAt (16, 16).getBrightness() > 0.6f);
    CHECK (mask.getPixelAt (16, 3).getAlpha() == 0);
    CHECK (mask.getPixelAt (16, 16).getAlpha() > 200);
    CHECK (mask.getPixelAt (16, 16).getBrightness() < 0.05f);
}
