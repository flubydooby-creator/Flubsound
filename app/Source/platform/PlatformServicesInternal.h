// Flubsound Pro - private helpers shared by the PlatformServices_*.cpp/.mm
// implementations. Not part of the public PlatformServices.h contract.
#pragma once

#include "PlatformServices.h"

#include <cstdint>
#include <string>

namespace flub::platform::detail
{
/** KeyChord::keyCode encodes F1..F24 VK-style: F1 = 0x70 ... F24 = 0x87. */
constexpr uint32_t kFunctionKeyBase = 0x70;
constexpr int kNumFunctionKeys = 24;

constexpr bool isLetterKey (uint32_t keyCode) noexcept { return keyCode >= 'A' && keyCode <= 'Z'; }
constexpr bool isDigitKey (uint32_t keyCode) noexcept { return keyCode >= '0' && keyCode <= '9'; }

constexpr bool isFunctionKey (uint32_t keyCode) noexcept
{
    return keyCode >= kFunctionKeyBase && keyCode < kFunctionKeyBase + static_cast<uint32_t> (kNumFunctionKeys);
}

/** 1..24 for F1..F24, 0 for anything else. */
constexpr int functionKeyNumber (uint32_t keyCode) noexcept
{
    return isFunctionKey (keyCode) ? static_cast<int> (keyCode - kFunctionKeyBase) + 1 : 0;
}

constexpr uint32_t kAllModifiers = KeyChord::Ctrl | KeyChord::Alt | KeyChord::Shift | KeyChord::Super;

/** Platform-independent sanity rules for a global shortcut:
      - the key must be A-Z, 0-9 or F1-F24 (the only codes KeyChord defines),
      - no unknown modifier bits,
      - letters and digits need at least one modifier other than Shift, otherwise
        the shortcut would swallow ordinary typing in every other application.
    F-keys may be used bare (e.g. F13-F24 on gaming keyboards / macro pads).
    On failure, 'reason' (if given) receives a user-presentable explanation. */
bool isValidChord (const KeyChord& chord, std::string* reason = nullptr);

/** Human-readable name of the key alone ("K", "7", "F13"); "" if invalid. */
std::string keyName (uint32_t keyCode);
} // namespace flub::platform::detail
