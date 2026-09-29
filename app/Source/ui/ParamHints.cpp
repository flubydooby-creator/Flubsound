#include "ParamHints.h"

#include <map>

namespace flub::app::ui::ParamHints
{
using namespace flub::param;

namespace
{
struct Hint
{
    const char* music;
    const char* gaming = nullptr; // nullptr: the Music text serves both modes
};

// clang-format off
const std::map<juce::String, Hint>& table()
{
    static const std::map<juce::String, Hint> t {
        // ---- Global ----
        { "input.gain", { "Level into the strip. Lower it if a hot source makes the limiter work hard; it does not change the output level target." } },
        { "output.gain", { "Turns the strip's output down after everything else. Use it to balance this strip against the others." } },
        { "mode", { "Music shapes songs (punch, width, warmth); Gaming brings out cues (footsteps, direction, voices). The macros change with it." } },
        { "boost", { "One knob for more: clarity and width first, then bass, loudness last, always watched by the safety governor.",
                     "One knob for more: detail and direction first, then impact, loudness last, always watched by the safety governor." } },
        { "macro.1", { "Punch: sharper drum hits. Brings out the attack of kicks and snares without making the mix louder.",
                       "Footsteps: lifts steps and movement when they happen, not the whole background, so you hear who is close." } },
        { "macro.2", { "Width: a wider stereo image and a little more space around instruments. Mono listeners still hear everything.",
                       "Positional: sharper left/right and front/back placement, so you can tell where a sound comes from." } },
        { "macro.3", { "Clarity: clearer vocals and cymbals, less mud in the low mids. Too much can sound thin or bright.",
                       "Impact: bigger explosions and gunshots: more low end and attack on loud events." } },
        { "macro.4", { "Loudness: denser and louder, like a finished master. Higher settings flatten the dynamics.",
                       "Detail: lifts quiet sounds (distant steps, reloads, ambience) closer to the loud ones." } },
        { "macro.5", { "Warmth: a softer, darker tone with a fuller low end and a touch of tube colour; highs roll off gently.",
                       "Voice & Score: clearer dialogue and music over effects, with less boom in the low mids." } },
        { "autolevel.on", { "Evens out loud and quiet sources over a few seconds, so switching songs or games needs less volume riding." } },
        { "autolevel.target", { "The level Auto Level aims for. Lower is calmer and leaves more room for peaks; higher is louder." } },
        { "bypass.matched", { "Bypass plays the original at the processed loudness, so a before/after comparison is not won by the louder side." } },
        { "bypass", { "Hear the original sound without any processing on this strip." } },
        { "latency.profile", { "Quality sounds best but adds delay; Low Latency responds fastest for games; Balanced sits in between." } },
        { "auto.preamp", { "Turns the input down by as much as your settings boost, so bass or EQ boosts do not push the limiter harder." } },
        { "auto.preampAllowance", { "How much boost the automatic preamp leaves in. More keeps the level up but lets the limiter work more." } },
        { "auto.preampHot", { "On already loud masters, also takes back the leftover boost and loudness, so the limiter stays quiet." } },

        // ---- Module switches ----
        { "gate.on", { "Removes steady hiss and hum between sounds (only in the Quality latency profile)." } },
        { "eq.on", { "Your own tone settings: ten bands to lift or cut any frequency range." } },
        { "dyneq.on", { "EQ bands that act only when a frequency gets too loud (or too quiet), e.g. taming harsh peaks." } },
        { "bass.on", { "Bass boost, bass you can hear on small speakers, and protection that keeps the boost clean." } },
        { "clarity.on", { "Presence, air, de-mud and drum attack: makes voices and details easier to hear." } },
        { "sat.on", { "Analogue-style colour: tape, tube or digital saturation adds warmth and density." } },
        { "spatial.on", { "Width, positional focus, room space and headphone crossfeed." } },
        { "virt.on", { "Turns 5.1 / 7.1 game audio into 3D sound on headphones. Stereo sources pass through unchanged." } },
        { "comp.on", { "Evens out the difference between loud and quiet parts: louder quiet parts, calmer loud parts." } },
        { "max.on", { "Makes the strip louder and keeps peaks under the ceiling. The last stage before your speakers." } },

        // ---- Noise gate ----
        { "gate.threshold", { "How far above the learned noise a sound must be to pass. Higher removes more noise but can cut soft sounds." } },
        { "gate.reduction", { "How much the noise is turned down between sounds. Less reduction sounds more natural." } },
        { "gate.attack", { "How fast the gate opens for a new sound. Too slow softens the start of words and hits." } },
        { "gate.release", { "How fast the noise comes back down after a sound. Too fast can sound choppy." } },
        { "gate.floorRise", { "How quickly the gate relearns the noise when it changes, e.g. a fan speeding up." } },
        { "gate.freeze", { "Stops learning the noise: keeps the current noise profile, useful when the noise is steady." } },

        // ---- EQ ----
        { "eq.output", { "Overall level after the EQ. Turn it down after big boosts to keep the same loudness." } },
        { "eq.#.on", { "Switches EQ band # on or off." } },
        { "eq.#.type", { "Band #'s shape: a bell for one range, shelves for everything above or below, filters to remove lows or highs." } },
        { "eq.#.freq", { "Where band # acts: low values for bass, around 1-4 kHz for voices, high values for air and hiss." } },
        { "eq.#.gain", { "How much band # lifts or cuts. Small moves (1-3 dB) are usually enough." } },
        { "eq.#.q", { "How wide band # is: low Q affects a broad range gently, high Q a narrow notch." } },
        { "eq.#.slope", { "How steep band # filters or shelves: steeper removes more just past the frequency." } },

        // ---- Dynamic EQ ----
        { "dyneq.#.on", { "Switches dynamic band # on or off." } },
        { "dyneq.#.mode", { "Whether dynamic band # turns its range down when it gets loud, or up when it gets quiet." } },
        { "dyneq.#.shape", { "Dynamic band #'s shape: a bell for one range, or a shelf for everything above or below it." } },
        { "dyneq.#.freq", { "Where dynamic band # listens and acts, e.g. 5-8 kHz for harsh 's' sounds." } },
        { "dyneq.#.q", { "How wide dynamic band # is: low Q for a broad range, high Q for one narrow resonance." } },
        { "dyneq.#.threshold", { "The level where dynamic band # starts to act. Lower acts more often." } },
        { "dyneq.#.ratio", { "How strongly dynamic band # reacts once past its threshold." } },
        { "dyneq.#.range", { "The most dynamic band # may cut or lift, so it never goes too far." } },
        { "dyneq.#.staticGain", { "A fixed lift or cut of dynamic band #'s range, on top of what it does dynamically." } },
        { "dyneq.#.attack", { "How fast dynamic band # reacts. Fast catches short peaks; slow sounds smoother." } },
        { "dyneq.#.release", { "How fast dynamic band # lets go after the sound drops." } },
        { "dyneq.#.noiseFloor", { "Below this level dynamic band # ignores the signal, so it does not lift hiss in quiet passages." } },

        // ---- Bass ----
        { "bass.boost", { "More low end. A little goes a long way; the protection below keeps big boosts from distorting." } },
        { "bass.freq", { "Where the bass boost centres: lower for sub rumble, higher for punchy bass guitar and kick." } },
        { "bass.protect", { "Keeps the bass boost from overloading: it backs the boost off on loud bass hits." } },
        { "bass.harmonics", { "Bass you can hear on small speakers and earbuds: adds overtones your ears read as low notes." } },
        { "bass.harmonicsCutoff", { "The lowest note your speakers can play. Harmonic bass fills in below it." } },
        { "bass.character", { "Harmonic bass tone: smoother and rounder at one end, grittier and more present at the other." } },
        { "bass.replaceFundamental", { "For tiny speakers: removes the deep notes they cannot play and keeps only the harmonic bass." } },
        { "bass.tighten", { "Shortens boomy bass notes so the low end sounds tighter and less muddy." } },
        { "bass.monoBelow", { "Makes the deepest bass mono, for a steadier, more centred low end on speakers and headphones." } },
        { "bass.subsonic", { "Removes rumble below hearing that wastes headroom. Raise it for small speakers." } },
        { "bass.subsonicOrder", { "How steeply rumble is removed. The gentler slope keeps deep bass tighter in time." } },
        { "bass.splitProtect", { "Protects deep bass and kick separately, so a steady bass line stops pumping with the kick drum." } },

        // ---- Clarity ----
        { "clarity.attack", { "More (or less) snap at the start of drum hits, plucks and steps." } },
        { "clarity.sustain", { "More (or less) of the ring after each hit: raise for fuller, lower for drier and tighter." } },
        { "clarity.presence", { "Brings voices and lead instruments forward. Too much sounds forward or shouty." } },
        { "clarity.presenceFreq", { "Where the presence lift sits: lower for body in voices, higher for bite and definition." } },
        { "clarity.presenceMode", { "Relative gives quiet and loud recordings the same presence, judged by how bright each one already is. Absolute adds more on quiet ones." } },
        { "clarity.air", { "Sparkle and openness on top. Too much can make cymbals and 's' sounds sharp." } },
        { "clarity.demud", { "Clears the boxy, muddy low mids that make a mix sound congested." } },

        // ---- Saturation ----
        { "sat.type", { "Tape is soft and warm, Tube adds rich even harmonics, Digital is harder and more aggressive." } },
        { "sat.drive", { "How hard the saturation is pushed: more colour and density, then audible distortion." } },
        { "sat.mix", { "Blend of saturated and clean sound. Lower keeps the detail of the original." } },
        { "sat.output", { "Level after the saturation, to match the level with it on and off." } },

        // ---- Stereo & space ----
        { "spatial.width", { "Makes the stereo image narrower or wider. Very wide can sound hollow on speakers." } },
        { "spatial.lowCut", { "Keeps the bass centred while the rest is widened, for a steady low end." } },
        { "spatial.focus", { "Sharpens where sounds come from, left to right, so positions are easier to pinpoint." } },
        { "spatial.space", { "Adds a sense of room around the sound, like speakers in a real space." } },
        { "spatial.crossfeed", { "Lets a little of each side reach the other ear, like speakers: less tiring on headphones." } },
        { "spatial.crossfeedType", { "Bs2b sounds like speakers in front; Meier is subtler; Mono-safe only narrows the bass and stays exact in mono." } },
        { "spatial.monoSafety", { "Pulls the width back when it would make parts cancel out in mono or on one speaker." } },
        { "spatial.minCorrelation", { "How far mono safety lets the sides differ before it steps in. Lower allows a wider image." } },

        // ---- Virtualiser ----
        { "virt.front", { "Where the virtual front speakers sit: wider spreads the front stage." } },
        { "virt.side", { "Where the virtual side speakers sit around your head." } },
        { "virt.rear", { "Where the virtual rear speakers sit: move them to make sounds behind you clearer." } },
        { "virt.headRadius", { "Match to your head size: it sets how far apart your ears are for the 3D effect." } },
        { "virt.room", { "How much virtual room: more sounds like speakers in a room, less is drier and more direct." } },
        { "virt.lfe", { "Level of the explosion rumble channel (LFE) in the headphone mix." } },
        { "virt.lfeFold", { "Mixes the rumble channel into your headphones at all. Off follows the plain stereo downmix." } },
        { "virt.input", { "Auto detects surround content; force surround or stereo if the game's channels are misdetected." } },
        { "virt.ownHrtf", { "Turn on when the game already makes 3D headphone sound itself, so it is not processed twice." } },

        // ---- Compressor ----
        { "comp.threshold", { "The level where the compressor starts turning loud parts down. Lower acts on more of the sound." } },
        { "comp.ratio", { "How strongly loud parts are turned down past the threshold: 2:1 is gentle, 8:1 is strong." } },
        { "comp.knee", { "How gradually compression starts around the threshold. A soft knee sounds more natural." } },
        { "comp.attack", { "How fast the compressor reacts. Slower lets drum hits through; faster catches every peak." } },
        { "comp.release", { "How fast the level comes back after a loud part. Too fast pumps, too slow sounds flat." } },
        { "comp.autoRelease", { "Adapts the release to the music: fast after short hits, slow after long loud parts." } },
        { "comp.makeup", { "Brings the compressed sound back up in level." } },
        { "comp.autoMakeup", { "Sets the makeup gain automatically so compression does not simply make things quieter." } },
        { "comp.scHp", { "Stops deep bass from triggering the compressor, so kicks do not duck the whole mix." } },
        { "comp.mix", { "Blend of compressed and original sound: lower keeps more of the original dynamics." } },
        { "comp.upThreshold", { "Sounds below this level get lifted by the upward compressor, bringing quiet details up." } },
        { "comp.upRatio", { "How strongly quiet sounds are lifted toward the upward threshold." } },
        { "comp.upMax", { "The most quiet sounds may be lifted. Keep it moderate so background noise does not come up too." } },
        { "comp.upFloor", { "Below this level nothing is lifted, so silence and hiss stay quiet." } },
        { "guard.range", { "How far a sudden loud event may rise over what you were hearing: smaller keeps blasts and screams in check." } },

        // ---- Maximizer ----
        { "max.drive", { "How hard the strip is pushed into the limiter: louder and denser, with less dynamics." } },
        { "max.ceiling", { "The highest peak level allowed out. Keep it below 0 dB so nothing clips after conversion." } },
        { "max.clip", { "How much of the peak control is soft clipping instead of limiting: louder, with a little more edge." } },
        { "max.clipKnee", { "How gently the clipper rounds peaks. Softer is smoother; harder keeps more punch." } },
        { "max.glue", { "Gently compresses the bands together for a cohesive, finished sound." } },
        { "max.release", { "How fast the limiter lets go. Faster is louder but can pump or distort bass." } },
        { "max.autoRelease", { "Adapts the limiter's release to the programme, for fewer pumping artefacts." } },
        { "max.autoDrive", { "Aims for the loudness target below instead of a fixed drive: steadier across quiet and loud sources." } },
        { "max.target", { "The loudness the maximizer aims for when Loudness Target is on." } },
        { "max.clipCrest", { "Clips only peaks that stick out far above the average, so dense parts stay clean." } },
        { "max.clipMaxDb", { "The most any peak may be clipped, so hard hits never get squashed too far." } },
        { "max.style", { "Ready-made limiter settings: Transparent, Punchy, Aggressive or Safe. Custom uses the knobs below." } },
        { "max.lfLimit", { "Limits deep bass on its own first, so kicks do not make the whole mix duck." } },
        { "max.bedLift", { "How much the maximizer may lift quiet background, so it does not raise ambience over the cues." } },

        // ---- Smoothness, contour, warmth ----
        { "smooth.amount", { "Takes back the harsh 's' and 't' sounds that Clarity and loudness add, without dulling the rest." } },
        { "contour.on", { "Keeps bass and treble in balance at low volume, where the ear hears less of them." } },
        { "contour.reference", { "How loud you listen when the tone sounds right. The contour adds bass as you go quieter than that." } },
        { "contour.level", { "How far below your reference volume you are listening now. Set by the system volume when following it." } },
        { "contour.maxLift", { "The most bass and treble the loudness contour may add at very low volume." } },
        { "warmth.tone", { "A warmer tilt: fuller lows, softer highs, at the same loudness." } },
        { "warmth.tapeGrit", { "Warmth as in older versions: tape saturation and extra bass instead of the gentle tilt." } },
    };
    return t;
}
// clang-format on

/** "eq.3.gain" -> "eq.#.gain" and band number 4; other keys unchanged, band 0. */
juce::String generalise (const juce::String& key, int& bandNumber)
{
    bandNumber = 0;
    const auto first = key.indexOfChar ('.');
    const auto second = first >= 0 ? key.indexOfChar (first + 1, '.') : -1;
    if (second < 0)
        return key;
    const auto middle = key.substring (first + 1, second);
    if (middle.isEmpty() || ! middle.containsOnly ("0123456789"))
        return key;
    bandNumber = middle.getIntValue() + 1;
    return key.substring (0, first + 1) + "#" + key.substring (second);
}
} // namespace

juce::String forKey (const juce::String& key, ModeValue mode)
{
    int band = 0;
    const auto general = generalise (key, band);
    const auto& t = table();
    const auto it = t.find (general);
    if (it == t.end())
        return {};
    const char* text = mode == ModeValue::Gaming && it->second.gaming != nullptr ? it->second.gaming : it->second.music;
    auto s = juce::String::fromUTF8 (text);
    return band > 0 ? s.replace ("#", juce::String (band)) : s;
}

bool differsByMode (const juce::String& key)
{
    int band = 0;
    const auto it = table().find (generalise (key, band));
    return it != table().end() && it->second.gaming != nullptr;
}

juce::String get (int id, ModeValue mode)
{
    if (id < 0 || id >= kNumParams)
        return {};
    return forKey (juce::String (layout()[static_cast<size_t> (id)].key), mode);
}

juce::String tooltip (int id, ModeValue mode, const juce::String& displayName)
{
    const auto name = displayName.isNotEmpty() ? displayName
                      : id >= 0 && id < kNumParams ? juce::String (layout()[static_cast<size_t> (id)].name)
                                                   : juce::String();
    const auto hint = get (id, mode);
    if (hint.isEmpty())
        return name;
    return name.isEmpty() ? hint : name + ": " + hint;
}
} // namespace flub::app::ui::ParamHints
