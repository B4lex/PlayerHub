#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // A player's menu at the crosshair, opened by activating the player's
    // character: the chat's player menu, with the mouse. Picking an item, a
    // click outside, Escape, or Activate closes it; Whisper opens the chat.
    [[nodiscard]] std::unique_ptr<Window> CreateWorldPlayerMenu();
}
