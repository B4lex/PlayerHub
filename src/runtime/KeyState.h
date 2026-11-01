#pragma once

#include <cstdint>

// Keyboard keys as the game's input dispatch reports them, for features that act on a key held
// during gameplay without taking it from the game.
namespace KeyState
{
    // Input thread. A press typed into a text field is not recorded; a release always is.
    void Record(std::uint32_t a_scanCode, bool a_pressed);

    [[nodiscard]] bool IsHeld(std::uint32_t a_scanCode);

    // Rises with each press, so a reader counts presses between its reads.
    [[nodiscard]] std::uint32_t GetPresses(std::uint32_t a_scanCode);
}
