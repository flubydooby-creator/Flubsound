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

/** Navigation keys use their VK codes too: Space 0x20, PageUp 0x21, PageDown
    0x22, End 0x23, Home 0x24, Left/Up/Right/Down 0x25..0x28, Insert 0x2D,
    Delete 0x2E. */
constexpr bool isNavigationKey (uint32_t keyCode) noexcept
{
    return (keyCode >= 0x20 && keyCode <= 0x28) || keyCode == 0x2D || keyCode == 0x2E;
}

constexpr uint32_t kAllModifiers = KeyChord::Ctrl | KeyChord::Alt | KeyChord::Shift | KeyChord::Super;

/** Platform-independent sanity rules for a global shortcut:
      - the key must be A-Z, 0-9, F1-F24 or a navigation key (the only codes
        KeyChord defines),
      - no unknown modifier bits,
      - letters, digits and navigation keys need at least one modifier other
        than Shift, otherwise
        the shortcut would swallow ordinary typing in every other application.
    F-keys may be used bare (e.g. F13-F24 on gaming keyboards / macro pads).
    On failure, 'reason' (if given) receives a user-presentable explanation. */
bool isValidChord (const KeyChord& chord, std::string* reason = nullptr);

/** Human-readable name of the key alone ("K", "7", "F13", "Up"); "" if
    invalid. The names match the settings file's chord syntax. */
std::string keyName (uint32_t keyCode);
} // namespace flub::platform::detail
