#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Shows the connection state, round-trip latency, and the plugin's average
    // frame time in a screen corner. Display only.
    [[nodiscard]] std::unique_ptr<Window> CreateStatusPanel();
}
