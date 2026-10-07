// Flubsound Pro - flubsound-cli: batch processor, loudness analyser and
// parameter / preset browser. JUCE-free: links flub::core only. This file
// parses the command line, prints help and dispatches; the commands live in
// Commands.cpp.
//
//   flubsound-cli process -i in.wav -o out.wav [render options]
//   flubsound-cli batch   -i <in dir> -o <out dir> [render options] [--jobs N]
//   flubsound-cli analyze -i file.wav [--bands] [--events] [--glitches] [--spatial] [--focus-ild] [--json]
//   flubsound-cli quality [chain options] [--json]
//   flubsound-cli soak    [chain options] [--minutes M] [--seed N] [--json]
//   flubsound-cli params  [--json]
//   flubsound-cli presets [--dir <dir>] [--json]
//   flubsound-cli demo    [--input file.wav] [--out <dir>]   (Demo.cpp)
//   flubsound-cli ctl     <action> [strip] [value]          (Ctl.cpp: the running app)
//
// Exit codes: 0 success, 1 processing / I/O failure (for batch: at least one
// file failed), 2 usage error (bad option, unknown preset or parameter).
//
// With --json the machine-readable result goes to stdout and all progress /
// human-readable text goes to stderr, so `... --json > result.json` works.

#include "CliOptions.h"
#include "Commands.h"
#include "Ctl.h"
#include "Demo.h"
#include "LatencyProbeCommand.h"
#include "Utf8Windows.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <vector>

#ifndef FLUB_CLI_VERSION
    #define FLUB_CLI_VERSION "0.0.0"
#endif

using namespace flub::cli;

namespace
{
// ===========================================================================
// Help
// ===========================================================================
const char* const kGeneralHelp = R"(flubsound-cli - Flubsound Pro batch processor and loudness analyser

Usage:
  flubsound-cli process -i in.wav -o out.wav [render options]
  flubsound-cli batch   -i <in dir> -o <out dir> [render options] [--jobs N] [--recursive]
  flubsound-cli analyze -i file.wav [--bands] [--events] [--glitches] [--spatial] [--focus-ild] [--json]
  flubsound-cli quality [preset / mode / macro / --set options] [--json]
  flubsound-cli soak    [preset / mode / macro / --set options] [--minutes M] [--json]
  flubsound-cli params  [--json]
  flubsound-cli presets [--dir <dir>] [--json]
  flubsound-cli demo    [--input file.wav] [--out <dir>] [--seconds S] [--jobs N]
                                      by-ear before / after pairs (`help demo`)
  flubsound-cli latency-probe generate|analyze ...   loopback latency (`latency-probe --help`)
  flubsound-cli ctl <action> [strip] [value]   control the running app (`ctl --help`)
  flubsound-cli help <command>        detailed help for one command
  flubsound-cli --version

Render options (process / batch; quality and soak take all but the file
options, --target-lufs and --format):
  -p, --preset <file.json|name>  preset file, or factory preset name (see `presets`)
      --preset-dir <dir>         factory preset folder for --preset <name>
  -m, --mode music|gaming        processing mode (selects the macro set)
  -b, --boost 0-100              Boost Intensity
      --macro N=0-100            mode macro, N = 1..5 or its name (punch=60,
                                 footsteps=40, ...); repeatable, "--macro 1=50 3=20"
  -s, --set key=value            any parameter (keys: `flubsound-cli params`);
                                 repeatable, "--set eq.0.gain=3 bass.freq=60Hz"
  -t, --target-lufs L            integrated loudness target (iterative render)
  -c, --ceiling dBTP             true-peak ceiling of the maximizer (-12..0)
      --profile quality|balanced|low-latency
                                 latency profile (default: from the preset,
                                 Balanced = what the real-time engine uses)
      --protection off|normal|strict
                                 SafetyGovernor reach (default off, like the
                                 app): normal also governs the base max.drive,
                                 sat.drive and bass.harmonics, strict lets the
                                 scale fall to 0
      --smart on|off             Smart macros (default: the preset's own
                                 "smart" flag, off without one): the content
                                 analysis scales what the macros add
      --neural voice-cleanup|off process / batch: the experimental neural voice
                                 cleanup (speech in noise) in the chain's
                                 neural slot; 48 kHz input only, adds 20 ms
  -f, --format f32|pcm24|pcm16   output sample format (default f32; PCM is
                                 TPDF dithered)
      --block N                  processing block size (default 512)
  -q, --quiet                    only print errors (and --json output)
      --json                     machine-readable result on stdout (process /
                                 batch: render.stats, see `help process`)
      --bands                    process / analyze: octave-band levels
      --events                   analyze: scene events (onsets, loud events,
                                 silences, level changes; see `help analyze`)
      --glitches                 analyze: clicks, dropouts, NaN / Inf, DC steps
      --spatial                  analyze: IACC, ITD, DRR, diffuse field of binaural impulses
      --focus-ild                analyze: ILD of a binaural source through the focus 0/50/100 %

Precedence: defaults < --preset < --mode < --boost/--macro/--profile/--ceiling
< --set. Bypass is never taken from a preset (use --set bypass=on).

Exit codes: 0 ok, 1 processing or I/O failure, 2 usage error.
)";

const char* const kProcessHelp = R"(flubsound-cli process -i in.wav -o out.wav [render options]

Renders one WAV file through the Flubsound processing chain - exactly the
code the real-time engine runs - and writes a stereo WAV of the same length
and sample rate.

  * Sample-aligned: the chain latency is compensated (the first L output
    samples are dropped and L samples of silence flush the tail), so the
    output lines up with the input sample for sample.
  * Channels: mono is duplicated to stereo; 5.1 (6 ch) and 7.1 (8 ch) files
    are virtualised binaurally (virt.on, default) or downmixed (ITU-R BS.775)
    to stereo, the LFE folded at virt.lfe re one main channel. Content only
    on FL/FR switches to a stereo passthrough after 2 s (virt.input Auto).
  * --target-lufs L: render, measure the integrated loudness (EBU R128),
    move max.drive (0..24 dB) by the error and render again - up to 4 more
    passes, stopping within 0.3 LU. Beyond 24 dB of drive input.gain is
    raised; below 0 dB of drive output.gain is lowered, then input.gain
    (down to -24 dB each). The maximizer's true-peak limiter holds the
    ceiling (--ceiling, default from the preset / -1 dBTP); a measured
    overshoot is trimmed off the delivered file.
  * Percent parameters are stored as 0..1: --set clarity.air=0.4 or =40%.
  * Render statistics of the delivered pass are printed ("Stats") and, with
    --json, written to render.stats: limiter / glue / compressor gain
    reduction (deepest, mean, time deeper than 1 / 3 dB), clip energy,
    measured THD+N, the intended harmonics of the bass harmonics / air
    exciter, bass protection, the dynamic EQ mode bands, the SafetyGovernor's
    Boost scale, state and reasons (time shares, and scale / state / reasons
    at the end: governor.end), at --protection normal / strict the measured
    loop (governor.measured: harmonics and tonal scales, audible residuals,
    output PLR, brightness, with their budgets), the Smoothness stage's cut
    (smoothness), and AutoLevel / AutoDrive, read from the chain's meters
    once per block.
  * --bands: octave-band levels (31.5 Hz .. 16 kHz, dBFS) of the input and
    the rendered output (inputBands / outputBands with --json).

Examples:
  flubsound-cli process -i song.wav -o song-fx.wav --preset "Punchy Pop" --boost 60
  flubsound-cli process -i song.wav -o master.wav --target-lufs -14 --ceiling -1 --format pcm24
  flubsound-cli process -i clip.wav -o clip-fx.wav --mode gaming --macro footsteps=70 --set eq.3.gain=-2
)";

const char* const kBatchHelp = R"(flubsound-cli batch -i <in dir> -o <out dir> [render options] [--jobs N] [--recursive]

Processes every .wav file of a folder with the same settings (see `help
process`). Non-WAV files are skipped. Output files keep their names (and,
with --recursive, their sub-folders) below the output folder, which is
created if needed and must differ from the input folder.

  -j, --jobs N       parallel jobs (default: number of CPU cores). Every job
                     has its own parameter store and processing chain.
  -r, --recursive    include sub-folders

A summary table (loudness in/out, true peak, passes, time) is printed at the
end; with --json the per-file results are written to stdout instead.
Exit code 1 if any file failed.

Example:
  flubsound-cli batch -i ./album -o ./album-fx --mode music --boost 40 --target-lufs -14 --jobs 4 --format pcm24
)";

const char* const kAnalyzeHelp = R"(flubsound-cli analyze -i file.wav [--bands] [--events [--event-band Hz]] [--glitches] [--spatial] [--focus-ild] [--json]

Measures a WAV file with the engine's meters:
  integrated loudness (LUFS, EBU R128 gating), loudness range (LU, EBU Tech
  3342), maximum momentary (400 ms) and short-term (3 s) loudness, true peak
  (dBTP, 4x oversampled), sample peak and RMS (dBFS; a full-scale sine reads
  -3.01 dBFS RMS), per channel as well, plus duration, rate and channels.
5.1 / 7.1 files use the BS.1770 channel weights (LFE excluded).
Values that cannot be measured (silence, < 400 ms) print as -inf / null.
--bands adds octave-band levels of the mean of all channels (31.5 Hz ..
16 kHz, dBFS RMS; RBJ band-passes about one octave wide, for comparing
renders rather than class-1 IEC 61260 filtering).

Content (docs/11 E34; "content" / "suggest" with --json), over the programme
above -70 dB RMS: PLR (sample peak over the K-weighted loudness, LU; a
limited master reads 6 - 8), crest (peak over RMS), spectral tilt (dB per
octave of the octave bands 125 Hz .. 8 kHz; pink noise 0, white +3) and the
8 kHz+ bands over 500 Hz - 2 kHz, the share below 100 Hz, M/S width (side
over mid) and correlation, spectral flux and onsets per second. Suggest:
what Smart macros would keep of the macros' attack, drive, bass and air on
this file (1 = all), with notes.

--events (docs/11 E60; for game and film captures) reads the programme in
10 ms frames of all channels against its own background (a slow floor that
rises at most 5 dB/s) and lists
  onset         the level 6 dB or more over the background for <= 300 ms
                (a step, a click, a short cue)
  loud          the peak 20 dB over the background or above -6 dBFS (shots,
                explosions; frames < 150 ms apart are one event)
  silence       under -70 dBFS for >= 300 ms
  level-change  the median level over 2 s after a frame differs by >= 6 dB
                from the median over 2 s before it (a scene or track change)
--event-band Hz reads the events in one band (RBJ band-pass, Q 1), e.g.
3200 for footsteps. --bands --events adds per octave band (of the mid, as
--bands) a level track in 100 ms frames, its 10th / 50th / 90th
percentiles and the band's own events (bandTracks with --json).

--glitches (docs/11 E53; for loopback captures and renders) runs the
discontinuity detector over every channel: clicks (the 4th-order difference
24 dB over its RMS on both sides and above -70 dBFS: impulses, steps,
skipped or repeated samples), dropouts (>= 0.5 ms of exact zeros starting
abruptly after programme), NaN / Inf runs and DC steps (the 2 Hz low-passed
signal moving >= -30 dBFS within 250 ms). It is most sensitive on tonal
programme (a test tone); on broadband noise only large breaks show.

--spatial (docs/11 E60; stereo files of binaural impulse responses, e.g.
an impulse per speaker at least 100 ms apart rendered by `process` with the
virtualiser) splits the file at the impulses and reports for each its IACC
(max |interaural cross-correlation| within +-1 ms; early 0-80 ms and late
after 80 ms from the direct sound, broadband and per octave 125 Hz .. 8 kHz),
the ITD (the lag of that maximum), the DRR (the first 2.5 ms against the
rest) and its 1/3-octave levels, and over all of them the diffuse-field
response (the power average of every ear) as a deviation from its own mean.
A dry centred source reads IACC 1; "--" / null: the window holds less than
-60 dB of the response (e.g. no reverberant tail).

--focus-ild (docs/11 E24; stereo files of a binaural or HRTF-rendered source)
runs the file through the positional focus alone (width 1, no space or
crossfeed) at off, 50 % and 100 % and lists per 1/3 octave (250 Hz .. 16 kHz)
the source's ILD (left over right) and how far each rendering moves it,
with the largest and the mean deviation over 1 - 8 kHz.
)";

const char* const kQualityHelp = R"(flubsound-cli quality [preset / mode / macro / --set options] [--json]

Measures the sound quality of a setting on pinned test stimuli (docs/11
E59): the stimuli are generated (48 kHz, fixed seeds), rendered through the
processing chain exactly as `process` would, and measured on the output mid.
The settings options are those of `process` (--preset, --mode, --boost,
--macro, --set, --ceiling, --profile, --protection, --smart, --block).

  THD+N    sines at 40 / 60 / 100 / 1000 Hz, -6 dBFS peak: everything but the
           fundamental, dB re the output
  IMD      50 + 63 Hz (a bass third, -12 dBFS peak each): products up to 5th
           order; SMPTE 60 Hz + 7 kHz 4:1: sidebands 7 kHz +- k x 60 Hz
  MTND     31-tone pink multitone at -24 / -18 / -12 dBFS RMS: everything but
           the tones, dB re the tones, with the output's integrated loudness
  Ducking  1 / 2 / 4 / 8 kHz probes (-26 dBFS peak each) under 55 Hz kicks
           (-6 dBFS peak, every 500 ms): per probe the dip (median - min),
           lift (max - median), p95 - p5 of its gain in 20 ms windows, the
           share of time > 1 dB down and the modulation at 2 Hz (kick rate)
  Kick     synthetic kick every 500 ms: output vs input power 0-10, 10-30
           and 40-60 ms after each onset, and the shift of the energy
           centroid of 0-150 ms (timing: a 3 ms delay reads +3 ms)
  Loudness pink noise at -18 dBFS RMS: integrated loudness in / out, true peak
  Hygiene  at --rate R (default 48000; the families above stay at 48 kHz):
           worst alias in 20 Hz .. 20 kHz of 1 / 5 / 7 / 10 kHz sines at
           -6 dBFS peak (dBc), output DC of 0.35 sin 100 Hz + 0.35 cos 200 Hz,
           and at 88.2 kHz and above the multitone's power >= 22 kHz

Examples:
  flubsound-cli quality --mode music --boost 100
  flubsound-cli quality --preset "Flubsound Signature" --json
  flubsound-cli quality --set max.drive=12 --json
  flubsound-cli quality --mode music --macro warmth=100 --profile low-latency --rate 44100
)";

const char* const kSoakHelp = R"(flubsound-cli soak [preset / mode / macro / --set options] [--minutes M | --seconds S]
                   [--seed N] [--automation off|user|all] [--interval ms] [--rate R] [--json]

Runs the processing chain for a long time on generated programme with
parameter automation and watches its output for discontinuities (docs/11
E53): the offline half of the soak, without devices.

  * Programme: seeded and licence-free, a 30 s cycle of 6 s scenes - music
    (bass, chords, a 997 Hz lead, kicks), game (a quiet bed, 3.2 kHz steps,
    shots, an explosion), speech-like syllables, a loud section that drives
    the maximizer, and a fade into 1 s of digital silence and back. It is
    smooth and tonal, so the detector reads it at full sensitivity; the
    input is watched too (a self-check).
  * Automation (--automation, default user; --interval, default 250 ms mean,
    +-50 %): one host action per interval between blocks - Boost or a macro,
    a gain / EQ / bass / clarity / width / compressor / drive parameter,
    a module on / off, the mode, a factory preset, bypass, an A/B switch.
    `all` sets any non-structural parameter to a random value instead.
    The latency profile never changes (it re-prepares the chain).
  * Watched: clicks, dropouts, NaN / Inf, DC steps on the stereo output
    (see `help analyze`, --glitches), each with the last automation action
    before it and [bypassed] when the global bypass was engaged; the
    output's peak; the wall time per block against its real-time budget
    (--block, default 512, at --rate, default 48000).

--seconds S / --minutes M: length (default 10 minutes). Exit code 0 if the
output has no discontinuity, 1 if it has one (or the programme itself read
as discontinuous). tools/scripts/soak.py runs the long soak over several
settings. Everything but the timing is deterministic for a seed.

Examples:
  flubsound-cli soak --minutes 60
  flubsound-cli soak --preset "Competitive FPS" --seconds 120 --seed 7 --json
  flubsound-cli soak --automation all --interval 50 --minutes 10
)";

const char* const kParamsHelp = R"(flubsound-cli params [--json]

Lists every parameter of the processing chain: key (for --set and preset
files), name, unit, range, default and choices. Percent values are stored as
0..1 and shown x100. --json prints the raw layout (stored units).
)";

const char* const kPresetsHelp = R"(flubsound-cli presets [--dir <dir>] [--json]

Lists the factory presets (presets/factory/*.json; sub-folders are ignored).
The folder is searched in this order: --dir, $FLUBSOUND_PRESET_DIR,
presets/factory next to the executable or up to four parent folders above it,
../share/flubsound/presets/factory, and the source tree the CLI was built from.
`--preset <name>` in process / batch resolves names against the same folder
(exact name, file name, then a unique loose prefix / substring match).
)";

const char* const kDemoHelp = R"(flubsound-cli demo [--input file.wav] [--out <dir>] [--seconds S] [--jobs N] [--format f32|pcm24|pcm16]

Renders the by-ear demo pack: before / after WAV pairs through the Flubsound
processing chain, one pair per feature, and index.txt, which says per pair
how both sides were set (`process --set` options), what changed in numbers
and what to listen for.

  Pairs (62): every macro of both modes 0 -> 100 % (Music: Punch, Width,
  Clarity, Loudness, Warmth; Gaming: Footsteps, Positional, Impact, Detail,
  Voice & Score), Punch and Impact at Boost 100, Boost 0 -> 50 and 0 -> 100
  in both modes, Smoothness, the shaper's Attack Low / High, Relative
  presence, the crossfeed and its types, the headphone virtualiser (on a 7.1
  scene), the Enhanced renderer and its front / back contrast, the loudness
  contour, Startle Guard, Night, the four maximizer styles, the genre and
  levelling presets, the module cards (noise gate, EQ, dynamic EQ, bass
  boost, harmonics, Tighten, saturation, Tape grit, compressor, automatic
  preamp, latency profile, protection) and the app's own settings (Smart
  macros, the headset enhancement cap, the safe speaker bass cap, a
  headphone correction, the per-ear profile, the hearing cap, the chat duck
  and ChatMix). docs/12-feature-guide.md names the pair of every control.

  * Programmes: built-in synthetic music, speech and a game scene (steps,
    gunshots, explosions, a voice line; a 7.1 version for the virtualiser),
    --seconds long (default 10, 0.5..120). --input uses your own file for
    every pair instead (the virtualiser pair keeps the 7.1 scene unless your
    file is 5.1 / 7.1).
  * Loudness-matched: only the louder file of a pair is turned down to the
    quieter's integrated loudness. The level features (Loudness macro,
    Startle Guard, Night) are left unmatched and the index says so.
  * Numbers: each written file is measured as `analyze --bands` reads it;
    "Band delta" is after minus before per octave band.
  * --out / -o: the pack folder (default ./flubsound-demo, created if
    needed; existing pack files are replaced). --format: default pcm24
    (TPDF dithered). --jobs: parallel renders (default: CPU cores). The same
    options give the same files, whatever --jobs is.

Examples:
  flubsound-cli demo
  flubsound-cli demo --input "My Song.wav" --out my-song-demo
)";

void printHelp (const std::string& topic, std::FILE* stream)
{
    std::string t = topic;
    std::transform (t.begin(), t.end(), t.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    const char* text = kGeneralHelp;
    if (t == "process")
        text = kProcessHelp;
    else if (t == "batch")
        text = kBatchHelp;
    else if (t == "analyze" || t == "analyse")
        text = kAnalyzeHelp;
    else if (t == "quality")
        text = kQualityHelp;
    else if (t == "soak")
        text = kSoakHelp;
    else if (t == "params" || t == "parameters")
        text = kParamsHelp;
    else if (t == "presets")
        text = kPresetsHelp;
    else if (t == "demo")
        text = kDemoHelp;
    std::fputs (text, stream);
}

/** "gcc 13.3.0", "clang 20.1.2", "appleclang ...", "msvc 1943": the first word is the ratchet's compiler key. */
const char* compilerDescription() noexcept
{
#if defined(_MSC_VER) && ! defined(__clang__)
    #define FLUB_STRINGIFY2(x) #x
    #define FLUB_STRINGIFY(x) FLUB_STRINGIFY2 (x)
    return "msvc " FLUB_STRINGIFY (_MSC_VER);
#elif defined(__apple_build_version__)
    return "appleclang " __clang_version__;
#elif defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "other";
#endif
}
} // namespace

// ===========================================================================
int main (int argc, char** argv)
{
    useUtf8Console();
    const std::vector<std::string> args = utf8Arguments (argc, argv);
    if (! args.empty() && args[0] == "latency-probe") // docs/11 E42d: parses its own options
        return runLatencyProbe ({ args.begin() + 1, args.end() });
    if (! args.empty() && args[0] == "ctl") // docs/11 E56: remote control of the running app (Ctl.h)
        return ctl::runCtl ({ args.begin() + 1, args.end() });

    CliOptions options;
    std::string error;
    if (! parseCommandLine (args, options, error))
    {
        std::fprintf (stderr, "error: %s\nRun `flubsound-cli --help` for usage.\n", error.c_str());
        return kExitUsage;
    }

    try
    {
        switch (options.command)
        {
            case Command::Help:
                if (args.empty())
                {
                    printHelp ({}, stderr); // bare invocation is a usage error
                    return kExitUsage;
                }
                printHelp (options.helpTopic, stdout);
                return kExitOk;
            case Command::Version:
                std::printf ("flubsound-cli %s (Flubsound Pro - Music & Gaming Edition)\n", FLUB_CLI_VERSION);
                // The compiler names the KNOWN_GAP ratchet values that apply
                // (tests/quality_targets.json, tools/scripts/quality-report.py).
                std::printf ("built with: %s\n", compilerDescription());
                return kExitOk;
            case Command::Process: return runProcess (options);
            case Command::Batch: return runBatch (options);
            case Command::Analyze: return runAnalyze (options);
            case Command::Quality: return runQuality (options);
            case Command::Soak: return runSoak (options);
            case Command::Params: return runParams (options);
            case Command::Presets: return runPresets (options);
            case Command::Demo: return runDemo (options);
            case Command::None: break;
        }
    }
    catch (const std::bad_alloc&)
    {
        std::fputs ("error: out of memory\n", stderr);
        return kExitFailure;
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return kExitFailure;
    }
    printHelp ({}, stderr);
    return kExitUsage;
}
