// Flubsound Pro - ITU-R BS.1770-4 channel weights for the engine's layouts.
//
// BS.1770-4 weights a channel 1.41 (+1.5 dB) when its loudspeaker sits
// between 60 and 120 degrees azimuth (below 30 degrees elevation), 1.0
// otherwise, and excludes the LFE. The engine uses the Windows
// KSAUDIO_SPEAKER_*_SURROUND / WAVEFORMATEXTENSIBLE channel orders (see
// HeadphoneVirtualizer.h):
//   <= 5 channels : no LFE / surround interpretation, every channel 1.0
//   5.1 (6 or 7)  : FL FR FC LFE SL SR [+ extra]   SL/SR (~110 deg) 1.41
//   7.1 (8+)      : FL FR FC LFE BL BR SL SR        BL/BR (~135-150 deg) 1.0,
//                                                   SL/SR (~90-110 deg) 1.41
// Shared by LoudnessMeter and LoudnessFollower so both read identically.
#pragma once

namespace flub
{
inline double bs1770ChannelWeight (int channel, int numChannels) noexcept
{
    if (channel < 0 || channel >= numChannels)
        return 0.0;
    if (numChannels < 6)
        return 1.0;
    if (channel == 3)
        return 0.0; // LFE
    if (numChannels < 8)
        return channel == 4 || channel == 5 ? 1.41 : 1.0; // 5.1 side surrounds
    return channel == 6 || channel == 7 ? 1.41 : 1.0;     // 7.1: back pair 1.0, sides 1.41
}
} // namespace flub
