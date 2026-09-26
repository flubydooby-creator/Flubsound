// Flubsound Pro - deterministic synthetic programme material.
//
// Used by the headless screenshot mode (and handy for demos / soak tests):
//   Music  : 120 BPM groove - kick, snare, hats, 8th-note bass line and a
//            detuned saw pad over an Am-F-C-G progression (stereo).
//   Game71 : 7.1 game-like scene (FL FR FC LFE BL BR SL SR) - footsteps
//            circling the listener, gunfire bursts, periodic explosions with
//            LFE, dialogue-like formant voice on the centre channel and a
//            decorrelated ambience bed on all main channels.
//
// Everything is generated sample by sample from a fixed seed, so two runs
// render identical audio. Not RT-critical (offline only), but allocation-free
// after construction.
#pragma once

#include "AudioEngineHost.h"

#include <array>
#include <memory>

namespace flub::app
{
class TestSignalGenerator final : public StripSignalSource
{
public:
    enum class Programme
    {
        Silence,
        Music,
        Game71
    };

    explicit TestSignalGenerator (double sampleRate);
    ~TestSignalGenerator() override;

    /** Assigns a programme to a strip (all strips start silent). A Game71
        programme on a stereo strip is downmixed; Music on a surround strip
        uses the front pair. */
    void setProgramme (int strip, Programme programme, float gainDb = 0.0f);

    bool renderStrip (int strip, const flub::AudioBlock& block) override;

private:
    class MusicSynth;
    class GameSynth;

    struct StripSource
    {
        Programme programme = Programme::Silence;
        float gain = 1.0f;
        std::unique_ptr<MusicSynth> music;
        std::unique_ptr<GameSynth> game;
    };

    double sampleRate;
    std::array<StripSource, AudioEngineHost::kMaxStrips> strips;
};
} // namespace flub::app
