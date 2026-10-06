#include "VisualiserRegistry.h"

#include "ChordView.h"
#include "ChromagramView.h"
#include "CorrelationMeter.h"
#include "GainReductionTrace.h"
#include "Goniometer.h"
#include "KeyView.h"
#include "LoudnessHistory.h"
#include "RadialSpectrum.h"
#include "StereoField.h"
#include "Waterfall3D.h"
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
        { "waterfall-3d", "3D waterfall (spectrum landscape)", "3D WATERFALL",
          "The output spectrum of the last 6 seconds as a landscape: low frequencies left, high right, louder is "
          "higher; the live spectrum is the glowing front ridge and older ones recede towards the horizon.",
          true, false, &make<Waterfall3D> },
        { "radial-spectrum", "Radial spectrum", "RADIAL SPECTRUM",
          "The output spectrum around a ring, mirrored left and right: lows at the top, highs at the bottom, louder "
          "reaches further out. The centre swells on kicks and shows the momentary loudness.",
          true, false, &make<RadialSpectrum> },
        { "correlation", "Correlation meter", "CORRELATION",
          "Phase correlation of the output's left and right channels: +1 mono, 0 unrelated (wide), below 0 "
          "(red) partly out of phase. The marker holds the lowest value of the last 3 seconds.",
          false, true, &make<CorrelationMeter> },
        { "chord", "Chord name", "CHORD",
          "The chord that is playing, named from the notes' fundamentals (not their overtones): big symbol, its notes, "
          "a keyboard lighting the pitch classes that sound and the last chords. Slash chords name the bass note.",
          true, true, &make<ChordView> },
        { "chromagram", "Chromagram (notes in all octaves)", "CHROMAGRAM",
          "How much of each note C .. B the output holds, all octaves folded together, with a 15-second history and "
          "the estimated key (its scale tones marked).",
          true, true, &make<ChromagramView> },
        { "key", "Song key", "KEY",
          "The estimated key of what is playing (major or minor, from about the last 15 seconds) with a confidence, "
          "the relative key and the scale's notes.",
          false, true, &make<KeyView> },
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
