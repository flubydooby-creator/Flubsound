#include "HotkeyCapture.h"

#include "Theme.h"
#include "platform/PlatformBridge.h"

namespace flub::app::ui
{
using flub::platform::KeyChord;

bool chordFromKeyPress (const juce::KeyPress& key, KeyChord& chord, const std::function<uint32_t (uint32_t character)>& keyForCharacter)
{
    chord = {};
    const int code = key.getKeyCode();
    uint32_t vk = 0;
    if (code >= 'a' && code <= 'z')
        vk = static_cast<uint32_t> (code - 'a' + 'A'); // Linux and macOS report letters in lower case
    else if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
        vk = static_cast<uint32_t> (code);
    else
    {
        // KeyChord's VK-style codes (PlatformServices.h); JUCE's codes for
        // these keys differ per OS, so compare with its constants.
        struct Named
        {
            int juceCode;
            uint32_t vk;
        };
        const Named named[] = {
            { juce::KeyPress::spaceKey, 0x20 }, { juce::KeyPress::pageUpKey, 0x21 }, { juce::KeyPress::pageDownKey, 0x22 },
            { juce::KeyPress::endKey, 0x23 },   { juce::KeyPress::homeKey, 0x24 },   { juce::KeyPress::leftKey, 0x25 },
            { juce::KeyPress::upKey, 0x26 },    { juce::KeyPress::rightKey, 0x27 },  { juce::KeyPress::downKey, 0x28 },
            { juce::KeyPress::insertKey, 0x2D }, { juce::KeyPress::deleteKey, 0x2E },
        };
        for (const auto& n : named)
            if (code == n.juceCode)
                vk = n.vk;

        const int functionKeys[] = {
            juce::KeyPress::F1Key,  juce::KeyPress::F2Key,  juce::KeyPress::F3Key,  juce::KeyPress::F4Key,  juce::KeyPress::F5Key,
            juce::KeyPress::F6Key,  juce::KeyPress::F7Key,  juce::KeyPress::F8Key,  juce::KeyPress::F9Key,  juce::KeyPress::F10Key,
            juce::KeyPress::F11Key, juce::KeyPress::F12Key, juce::KeyPress::F13Key, juce::KeyPress::F14Key, juce::KeyPress::F15Key,
            juce::KeyPress::F16Key, juce::KeyPress::F17Key, juce::KeyPress::F18Key, juce::KeyPress::F19Key, juce::KeyPress::F20Key,
            juce::KeyPress::F21Key, juce::KeyPress::F22Key, juce::KeyPress::F23Key, juce::KeyPress::F24Key,
        };
        for (uint32_t i = 0; i < 24; ++i)
            if (code == functionKeys[i])
                vk = 0x70 + i;

        // Another character: the letter or digit key that types it on this
        // layout (JUCE names a key by its unshifted character on Windows, so
        // AZERTY's 1 key arrives as '&' and a Cyrillic B key as its letter).
        // JUCE's codes for keys without a character are 0x10000 and above.
        if (vk == 0 && code > 0x20 && code < 0x10000)
        {
            const auto character = static_cast<uint32_t> (code);
            const auto mapped = keyForCharacter != nullptr ? keyForCharacter (character) : platform_bridge::keyCodeForCharacter (character);
            if ((mapped >= 'A' && mapped <= 'Z') || (mapped >= '0' && mapped <= '9'))
                vk = mapped;
        }
    }
    if (vk == 0)
        return false;

    const auto mods = key.getModifiers();
    uint32_t modifiers = KeyChord::None;
    if (mods.isCtrlDown())
        modifiers |= KeyChord::Ctrl;
   #if JUCE_MAC
    // On macOS ctrlModifier is the Control key and commandModifier Cmd; on
    // Windows and Linux they are the same flag (and JUCE does not report the
    // Win / Super key, so Super chords are typed or picked, not recorded).
    if (mods.isCommandDown())
        modifiers |= KeyChord::Super;
   #endif
    if (mods.isAltDown())
        modifiers |= KeyChord::Alt;
    if (mods.isShiftDown())
        modifiers |= KeyChord::Shift;

    chord.modifiers = modifiers;
    chord.keyCode = vk;
    return true;
}

juce::String describeHeldModifiers (const juce::ModifierKeys& mods)
{
    juce::String text;
    if (mods.isCtrlDown())
        text << "Ctrl+";
    if (mods.isAltDown())
        text << "Alt+";
    if (mods.isShiftDown())
        text << "Shift+";
   #if JUCE_MAC
    if (mods.isCommandDown())
        text << "Super+";
   #endif
    return text;
}

// =============================================================================
HotkeyCaptureField::HotkeyCaptureField (const juce::String& name)
    : juce::Button (name + " shortcut"), actionName (name)
{
    setTitle (name + " shortcut");
    setTooltip ("Click (or Return / Space), then press the new shortcut, for example Ctrl+Alt+Shift+B. Esc cancels, Backspace clears it. "
                "Right-click or Shift+F10 to type it instead (Win / Super key chords).");
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
}

void HotkeyCaptureField::setChordText (const juce::String& newChordText)
{
    if (chordText == newChordText)
        return;
    chordText = newChordText;
    setDescription (actionName + ": " + chordText);
    repaint();
}

void HotkeyCaptureField::setProblem (bool hasProblem)
{
    if (problem == hasProblem)
        return;
    problem = hasProblem;
    repaint();
}

juce::String HotkeyCaptureField::getDisplayText() const
{
    if (! capturing)
        return chordText;
    const auto prefix = describeHeldModifiers (held);
    return prefix.isEmpty() ? juce::String ("Press a shortcut...") : prefix + "...";
}

void HotkeyCaptureField::startCapture()
{
    if (capturing)
        return;
    capturing = true;
    held = juce::ModifierKeys::getCurrentModifiers().withoutMouseButtons();
    if (isShowing())
    {
        grabKeyboardFocus();
        startTimerHz (20);
    }
    repaint();
    if (onCaptureStarted != nullptr)
        onCaptureStarted();
}

void HotkeyCaptureField::cancelCapture()
{
    if (capturing)
        endCapture();
}

void HotkeyCaptureField::timerCallback()
{
    const auto now = juce::ModifierKeys::getCurrentModifiersRealtime().withoutMouseButtons();
    if (now != held)
    {
        held = now;
        repaint();
    }
}

void HotkeyCaptureField::endCapture()
{
    stopTimer();
    capturing = false;
    held = {};
    repaint();
    if (onCaptureEnded != nullptr)
        onCaptureEnded();
}

void HotkeyCaptureField::clicked()
{
    startCapture();
}

void HotkeyCaptureField::clicked (const juce::ModifierKeys& modifiers)
{
    if (modifiers.isPopupMenu() && ! capturing && onTypeRequested != nullptr)
    {
        onTypeRequested();
        return;
    }
    clicked();
}

bool HotkeyCaptureField::keyPressed (const juce::KeyPress& key)
{
    if (! capturing)
    {
        const auto mods = key.getModifiers().withoutMouseButtons();
        const bool plain = ! mods.isAnyModifierKeyDown();
        if (plain && (key.isKeyCode (juce::KeyPress::returnKey) || key.isKeyCode (juce::KeyPress::spaceKey)))
        {
            startCapture(); // juce::Button only knows Return, and clicks asynchronously
            return true;
        }
        if (key.isKeyCode (juce::KeyPress::F10Key) && mods == juce::ModifierKeys (juce::ModifierKeys::shiftModifier) && onTypeRequested != nullptr)
        {
            onTypeRequested(); // the keyboard's context menu: typed entry
            return true;
        }
        return juce::Button::keyPressed (key);
    }

    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        cancelCapture();
        return true;
    }

    const auto mods = key.getModifiers();
    if (key.isKeyCode (juce::KeyPress::backspaceKey) && ! mods.isCtrlDown() && ! mods.isAltDown() && ! mods.isShiftDown()
        && ! mods.isCommandDown())
    {
        if (onCleared != nullptr)
            onCleared();
        endCapture();
        return true;
    }

    KeyChord chord;
    if (! chordFromKeyPress (key, chord))
    {
        if (onUnsupportedKey != nullptr)
            onUnsupportedKey (key.getTextDescription());
        return true; // still recording
    }
    if (onChordPressed != nullptr && onChordPressed (chord))
        endCapture();
    return true;
}

void HotkeyCaptureField::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    if (capturing)
    {
        held = mods.withoutMouseButtons();
        repaint();
        return;
    }
    juce::Button::modifierKeysChanged (mods);
}

void HotkeyCaptureField::focusLost (FocusChangeType cause)
{
    cancelCapture();
    juce::Button::focusLost (cause);
}

std::unique_ptr<juce::AccessibilityHandler> HotkeyCaptureField::createAccessibilityHandler()
{
    juce::AccessibilityActions actions;
    actions.addAction (juce::AccessibilityActionType::press, [this] { startCapture(); });
    actions.addAction (juce::AccessibilityActionType::showMenu,
                       [this]
                       {
                           if (! capturing && onTypeRequested != nullptr)
                               onTypeRequested();
                       });
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button, std::move (actions));
}

void HotkeyCaptureField::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Palette::well);
    g.fillRoundedRectangle (r, Theme::kControlRadius);

    const auto frame = capturing ? Theme::accent (*this)
                       : problem ? Theme::statusColours (*this).hot
                                 : (highlighted || hasKeyboardFocus (false) ? Palette::borderStrong : Palette::border);
    g.setColour (frame);
    g.drawRoundedRectangle (r, Theme::kControlRadius, capturing || problem ? 1.5f : 1.0f);

    g.setColour (capturing ? Palette::muted : Palette::text);
    g.setFont (Theme::font (13.0f));
    g.drawFittedText (getDisplayText(), getLocalBounds().reduced (8, 0), juce::Justification::centredLeft, 1, 0.85f);
}
} // namespace flub::app::ui
