#pragma once

#include <cstdint>

// Keys and mouse buttons as the game's input dispatch reports them, for features that act on a
// key held during gameplay without taking it from the game.
namespace KeyState
{
    // A key code is a DirectInput scan code for the keyboard, or kMouseBase plus the
    // BSWin32MouseDevice::Key for a mouse button, the numbering SKSE and MCM use.
    inline constexpr std::uint32_t kMouseBase = 256;
    // DirectInput's DIMOUSESTATE2 buttons; the wheel has no held state.
    inline constexpr std::uint32_t kMouseButtons = 8;

    [[nodiscard]] constexpr std::uint32_t FromMouse(std::uint32_t a_button) { return kMouseBase + a_button; }

    // Input thread. A press typed into a text field is not recorded; a release always is.
    void Record(std::uint32_t a_key, bool a_pressed);

    [[nodiscard]] bool IsHeld(std::uint32_t a_key);
}
