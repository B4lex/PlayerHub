#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Global chat in the lower-left corner. Enter opens it for typing; Enter
    // sends and closes, Escape cancels. The mouse wheel and Page Up/Down scroll
    // the history. While closed, new lines show briefly and fade out.
    [[nodiscard]] std::unique_ptr<Window> CreateChatWindow();
}
