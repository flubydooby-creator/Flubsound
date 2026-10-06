#include "VisualiserRegistry.h"

#include "CorrelationMeter.h"
#include "GainReductionTrace.h"
#include "Goniometer.h"
#include "LoudnessHistory.h"
#include "StereoField.h"
#include "WaveformView.h"

namespace flub::app::ui::vis
{
const std::vector<Descriptor>& registry()
{
    // To add a view: one line here (id, menu name, caption, description, main, strip, factory).
    static const std::vector<Descriptor> list {
        { "goniometer", "Goniometer (vectorscope)", "GONIOMETER",
          "The output's stereo picture as a Lissajous trace: mono is a vertical line, wide sound a round cloud, "
          "out-of-phase sound a horizontal line. Scaled automatically (the gain is shown).",
          true, false, &make<Goniometer> },
        { "stereo-field", "Stereo field by frequency", "STEREO FIELD",
          "Where each frequency sits between left and right: one dot per third of an octave, low at the bottom. "
          "A bar instead of a dot means that band is wide; brighter means louder.",
          true, false, &make<StereoField> },
        { "loudness-history", "Loudness history (60 s)", "LOUDNESS HISTORY",
          "Momentary (thin) and short-term (bold) loudness of the output over the last minute, in LUFS, "
          "with the loudness target when one is in force.",
          true, false, &make<LoudnessHistory> },
        { "waveform", "Waveform before / after", "WAVEFORM",
          "The input (grey) and output (accent) peak envelope over the last 4 seconds: what Boost, Punch and the "
          "limiter do to the peaks.",
          true, false, &make<WaveformView> },
        { "gain-reduction", "Gain reduction history", "GAIN REDUCTION",
          "How much the compressor, limiter, glue, bass protection and master limiter turn the sound down over "
          "the last 15 seconds.",
          true, false, &make<GainReductionTrace> },
        { "correlation", "Correlation meter", "CORRELATION",
          "Phase correlation of the output's left and right channels: +1 mono, 0 unrelated (wide), below 0 "
          "(red) partly out of phase. The marker holds the lowest value of the last 3 seconds.",
          false, true, &make<CorrelationMeter> },
    };
    return list;
}

const Descriptor* findDescriptor (const juce::String& id)
{
    const int i = indexOf (id);
    return i < 0 ? nullptr : &registry()[static_cast<size_t> (i)];
}

int indexOf (const juce::String& id)
{
    const auto& list = registry();
    for (size_t i = 0; i < list.size(); ++i)
        if (id == list[i].id)
            return static_cast<int> (i);
    return -1;
}

bool isValidId (const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 40 && id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789-");
}
} // namespace flub::app::ui::vis
