#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Chat in the lower-left corner. Enter opens it for typing; Enter sends and
    // closes, Escape cancels. A chip beside the text shows where the line
    // goes, filters turn channels on and off, and the mouse works once it
    // moves. While closed, new lines show briefly and fade out.
    [[nodiscard]] std::unique_ptr<Window> CreateChatWindow();
}
