// Flubsound Pro - AutoEQ ParametricEQ.txt / Equalizer APO (Peace) text import
// and export for the device correction (docs/11 E15).
//
// Accepted (case-insensitive, one command per line, '#' starts a comment):
//   Preamp: <gain> dB                     adds to the selected channels' gain
//   Filter[ <n>]: ON|OFF <type> ...       OFF filters are skipped
//     PK / PEQ / Modal  Fc <f> Hz Gain <g> dB Q <q> | BW Oct <bw>
//     LSC / HSC / LS / HS  Fc <f> Hz Gain <g> dB [Q <q>]     shelf at its centre
//                           frequency, RBJ Q (0.7071 when absent: warning)
//     LSC / HSC <s> dB ...  slope form: RBJ S = s / 12, converted to Q
//     LP / HP [Q <q>], LPQ / HPQ Q <q>   (Q 0.7071 when absent)
//     BP / NO / AP  Fc <f> Hz Q <q> | BW Oct <bw>
//   Channel: L R | 1 2 | all ...          selects the channels that follow;
//                                         C / LFE / surround channels have no
//                                         stereo output: their lines are
//                                         dropped with a warning
//   Device: ...                           ignored with a warning (the
//                                         correction belongs to the endpoint
//                                         it is imported for)
// Refused with a clear message: GraphicEQ (needs the FIR convolver),
// Include, Convolution, Stage, Delay, Copy, Eval and the If / Else family,
// the corner-frequency shelves "LS 6dB" / "LS 12dB" / "HS 6dB" / "HS 12dB"
// and anything else not listed. Limits: 16 filters per channel, |gain|
// (each filter, and each channel's Preamp lines added up)
// <= 30 dB, 1 Hz <= Fc <= 100 kHz, 0 < Q <= 100.
//
// Numbers are parsed locale-independently ('.' decimal point; a ',' is
// accepted when the token has no '.').
//
// format() writes the APO syntax back ("Filter n: ON PK Fc ... Q ..."), with
// "Channel:" / "Preamp:" lines where needed; parse (format (c)) == c exactly
// for every curve parse() can produce.
// The app persists each endpoint's correction in this form.
#pragma once

#include "flub/dsp/DeviceCorrection.h"

#include <string>
#include <string_view>
#include <vector>

namespace flub::eqtext
{
struct ParseResult
{
    bool ok = false;
    std::string error;                 // first error ("line 3: ...") when !ok
    std::vector<std::string> warnings; // what was ignored or assumed ("line 7: ...")
};

/** Parses `text` into `curve` (replaced on success, untouched on failure).
    Pure; any thread. */
ParseResult parse (std::string_view text, CorrectionCurve& curve);

/** APO / AutoEQ syntax for `curve` (see above). Pure; any thread. */
std::string format (const CorrectionCurve& curve);
} // namespace flub::eqtext
