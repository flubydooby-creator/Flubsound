// Flubsound Pro - the list of visualiser views (docs/06 §6.4.2).
//
// The registry is the only place that knows every view: the View menu lists
// its entries in this order, ui.analyzer stores their ids, VisualiserHost
// creates them, and the screenshot driver accepts "vis-<id>" for each.
#pragma once

#include "Visualiser.h"

#include <vector>

namespace flub::app::ui::vis
{
/** Every view, in menu order (VisualiserRegistry.cpp). */
const std::vector<Descriptor>& registry();

/** The entry with this id, or nullptr. */
const Descriptor* findDescriptor (const juce::String& id);

/** The entry's index in registry(), or -1. */
int indexOf (const juce::String& id);

/** True if `id` is usable as a persisted id: 1..40 characters of a-z, 0-9 and '-'. */
bool isValidId (const juce::String& id);
} // namespace flub::app::ui::vis
