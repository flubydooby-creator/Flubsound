// Flubsound Pro - plain-language hints: one "what you will hear" sentence per
// parameter (docs/11 E39).
//
// Keyed by the parameter's stable preset key (param::Info::key, e.g.
// "bass.harmonics"), not by English display names in core param::Info, so a
// translation workflow (docs/11 E41) can replace the table without touching
// the engine. Banded keys use '#' for the band number ("eq.#.gain"). Where a
// control means something different per mode (the five macros, Boost), the
// entry has a Gaming text next to the Music one; everywhere else one text
// serves both modes. Every text is at most kMaxLength characters
// (tests/app/test_app_ui_hints.cpp checks every key of the layout).
//
// Shown as the tooltip of every control bound to a parameter: the Boost
// dial and macros, the module cards' key controls and their expanded grids.
#pragma once

#include "flub/engine/Parameters.h"

#include <juce_core/juce_core.h>

namespace flub::app::ui::ParamHints
{
inline constexpr int kMaxLength = 140;

/** The hint for parameter `id` in `mode`; empty for an unknown id. */
juce::String get (int id, flub::param::ModeValue mode);

/** The hint for a preset key ("eq.3.gain"); empty when the table has none. */
juce::String forKey (const juce::String& key, flub::param::ModeValue mode);

/** True when the key has its own Gaming text (the macros, Boost). */
bool differsByMode (const juce::String& key);

/** "<display name>: <hint>" for a tooltip (the name alone without a hint). */
juce::String tooltip (int id, flub::param::ModeValue mode, const juce::String& displayName = {});
} // namespace flub::app::ui::ParamHints
