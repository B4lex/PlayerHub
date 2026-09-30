#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Chat in the lower-left corner. Enter opens it for typing; Enter sends and
    // closes, Escape cancels. Sender names are colored by channel. The mouse wheel and Page Up/Down scroll
    // the history. While closed, new lines show briefly and fade out.
    [[nodiscard]] std::unique_ptr<Window> CreateChatWindow();
}
